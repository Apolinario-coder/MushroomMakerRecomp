using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Media.Animation;
using System.Web.Script.Serialization;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("MushroomMakerRecomp")]
[assembly: System.Reflection.AssemblyProduct("MushroomMakerRecomp")]

static class Launcher {
    static string root = AppDomain.CurrentDomain.BaseDirectory;
    static string data = DataDirectory();
    static string DataDirectory() {
        string local=Environment.GetEnvironmentVariable("LOCALAPPDATA")??Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string current=Path.Combine(local,"MushroomMakerRecomp"),legacy=Path.Combine(local,"MarioMaker2PC");
        if(Directory.Exists(current) || !Directory.Exists(legacy))return current;
        try {
            using(var guard=new FileStream(Path.Combine(legacy,"session.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.ReadWrite|FileShare.Delete)) {
                if(guard.Length==0){guard.WriteByte(0);guard.Flush();}
                guard.Lock(0,1);
                guard.Unlock(0,1);
            }
            Directory.Move(legacy,current);
        } catch(IOException){return Directory.Exists(current)?current:legacy;}
          catch(UnauthorizedAccessException){return Directory.Exists(current)?current:legacy;}
        return current;
    }
    static Window window;
    static TextBox dump, details;
    static TextBlock status;
    static Button check, prepare, play;
    static ProgressBar progress;
    static bool busy, hasProgress;
    static int progressStep;
    static long progressDone, progressTotal;
    static StreamWriter report;
    static readonly object reportLock=new object();
    static string failure;
    static Dictionary<string,double> timeline;
    static bool motionEnabled, rendering;
    static readonly string[] panels={"HeaderPanel","HeroPanel","FormPanel","HelpPanel","FooterPanel"};
    static readonly string[] beats={"header","hero","form","help","footer"};
    static KeySpline EaseOut(){return new KeySpline(.16,1,.3,1);}
    static void Animate(DependencyObject target,DependencyProperty property,double from,double to,double duration,double delay) {
        var animated=(IAnimatable)target;
        animated.BeginAnimation(property,null);target.SetValue(property,to);
        if(!motionEnabled || rendering)return;
        target.SetValue(property,from);
        var tween=new DoubleAnimationUsingKeyFrames {Duration=TimeSpan.FromSeconds(duration),BeginTime=TimeSpan.FromSeconds(delay)};
        tween.KeyFrames.Add(new LinearDoubleKeyFrame(from,KeyTime.FromTimeSpan(TimeSpan.Zero)));
        tween.KeyFrames.Add(new SplineDoubleKeyFrame(to,KeyTime.FromTimeSpan(TimeSpan.FromSeconds(duration)),EaseOut()));
        animated.BeginAnimation(property,tween);
    }
    static void EntryPose(double seconds) {
        for(int i=0;i<panels.Length;i++) {
            var panel=Get<FrameworkElement>(panels[i]);
            double p=motionEnabled?Math.Max(0,Math.Min(1,(seconds-timeline["enter."+beats[i]])/timeline["enter."+beats[i]+".duration"])):1;
            double settled=EaseOut().GetSplineProgress(p);
            panel.Opacity=.86+.14*settled;
            panel.RenderTransform=new TranslateTransform(0,(i==0?4:10)*(1-settled));
        }
    }
    static void Enter() {
        EntryPose(0);
        for(int i=0;i<panels.Length;i++) {
            var panel=Get<FrameworkElement>(panels[i]);
            double delay=timeline["enter."+beats[i]],duration=timeline["enter."+beats[i]+".duration"];
            Animate(panel,UIElement.OpacityProperty,.86,1,duration,delay);
            Animate(panel.RenderTransform,TranslateTransform.YProperty,i==0?4:10,0,duration,delay);
        }
    }
    static void ButtonMotion(Button button,bool pressed) {
        var transforms=(TransformGroup)button.RenderTransform;
        var scale=(ScaleTransform)transforms.Children[0];var position=(TranslateTransform)transforms.Children[1];
        bool hover=button.IsEnabled && (button.IsMouseOver || button.IsKeyboardFocused);
        double size=!button.IsEnabled?1:pressed?.985:hover?(button==play?1.025:1.01):1;
        double y=!button.IsEnabled?0:pressed?1:hover?-2:0;
        double duration=timeline[pressed?"interaction.press":"interaction.hover"];
        Animate(scale,ScaleTransform.ScaleXProperty,scale.ScaleX,size,duration,0);
        Animate(scale,ScaleTransform.ScaleYProperty,scale.ScaleY,size,duration,0);
        Animate(position,TranslateTransform.YProperty,position.Y,y,duration,0);
    }
    static void HookMotion() {
        foreach(string name in new[]{"Check","Prepare","Play","BrowseDump"}) {
            var button=Get<Button>(name);var transforms=new TransformGroup();transforms.Children.Add(new ScaleTransform(1,1));transforms.Children.Add(new TranslateTransform());
            button.RenderTransform=transforms;button.RenderTransformOrigin=new Point(.5,.5);
            button.MouseEnter+=(s,e)=>ButtonMotion(button,false);button.MouseLeave+=(s,e)=>ButtonMotion(button,false);
            button.GotKeyboardFocus+=(s,e)=>ButtonMotion(button,false);button.LostKeyboardFocus+=(s,e)=>ButtonMotion(button,false);
            button.PreviewMouseLeftButtonDown+=(s,e)=>ButtonMotion(button,true);button.PreviewMouseLeftButtonUp+=(s,e)=>ButtonMotion(button,false);
            button.PreviewKeyDown+=(s,e)=>{if(e.Key==System.Windows.Input.Key.Space || e.Key==System.Windows.Input.Key.Enter)ButtonMotion(button,true);};
            button.PreviewKeyUp+=(s,e)=>ButtonMotion(button,false);
            button.IsEnabledChanged+=(s,e)=>ButtonMotion(button,false);
        }
        // The main layout has no scrolling; the log follows new output without accepting wheel scrolling.
        details.PreviewMouseWheel+=(s,e)=>e.Handled=true;
        window.ContentRendered+=(s,e)=>Enter();
    }
    static void MeasureLayout(double width,double height) {
        var visual=(FrameworkElement)window.Content;
        visual.Measure(new Size(width,height));visual.Arrange(new Rect(0,0,width,height));visual.UpdateLayout();
    }
    static void CheckLayout(double width,double height) {
        EntryPose(timeline["review.settled"]);MeasureLayout(width,height);
        var rootVisual=(Visual)window.Content;
        foreach(string name in new[]{"Dump","BrowseDump","Check","Prepare","Play","StatusBox","Details"}) {
            var element=Get<FrameworkElement>(name);
            Rect bounds=element.TransformToAncestor(rootVisual).TransformBounds(new Rect(0,0,element.ActualWidth,element.ActualHeight));
            if(bounds.Left < -.1 || bounds.Top < -.1 || bounds.Right>width+.1 || bounds.Bottom>height+.1 || bounds.Width<1 || bounds.Height<1)
                throw new Exception("Layout bounds failed for "+name+" at "+width+" x "+height);
        }
        if(Get<FrameworkElement>("HelpPanel").ActualHeight<400)throw new Exception("Help panel has insufficient layout space.");
    }
    static T Get<T>(string name) where T:class { return window.FindName(name) as T; }
    static string Quote(string s) {
        // CommandLineToArgvW escaping also handles a folder ending in a backslash.
        var b = new StringBuilder("\""); int slash=0;
        foreach(char c in s) {
            if(c=='\\') { slash++; continue; }
            if(c=='"') b.Append('\\', slash*2+1); else b.Append('\\', slash);
            slash=0; b.Append(c);
        }
        b.Append('\\', slash*2); return b.Append('"').ToString();
    }
    static void Browse(TextBox target) {
        using(var dialog = new Forms.FolderBrowserDialog()) {
            dialog.Description = "Choose a folder";
            if(Directory.Exists(target.Text)) dialog.SelectedPath=target.Text;
            if(dialog.ShowDialog()==Forms.DialogResult.OK) target.Text=dialog.SelectedPath;
        }
    }
    static void SetBusy(bool value) {
        busy=value;
        foreach(string name in new[]{"Check","Prepare","Play","BrowseDump","Dump"})
            Get<Control>(name).IsEnabled=!value;
        UpdateBusyMotion();
    }
    static void UpdateBusyMotion() {
        bool unknown=busy && progressTotal==0;
        progress.IsIndeterminate=unknown && motionEnabled;
        progress.Value=progressTotal>0?100.0*progressDone/progressTotal:0;
        Get<FrameworkElement>("ProgressPanel").Visibility=busy || hasProgress?Visibility.Visible:Visibility.Collapsed;
        Get<TextBlock>("ProgressPercent").Text=progressTotal>0?Math.Floor(progress.Value).ToString()+"%":busy?"Working...":"";
    }
    static void ResetProgress() {
        hasProgress=false;progressStep=0;progressDone=0;progressTotal=0;
        progress.Foreground=(Brush)window.Resources["BuilderYellow"];
        Get<TextBlock>("ProgressText").Text="Waiting to start...";
        UpdateBusyMotion();
    }
    static void Activity(string text) {
        details.AppendText(text+Environment.NewLine);
        if(details.Text.Length>12000)details.Text=details.Text.Substring(details.Text.Length-6000);
        details.ScrollToEnd();
    }
    static bool ApplyProgress(string json) {
        try {
            var item=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(json);
            int step=Convert.ToInt32(item["step"]),steps=Convert.ToInt32(item["steps"]);
            long done=Convert.ToInt64(item["completed"]),total=Convert.ToInt64(item["total"]);
            if(step<1 || steps<step || steps>100 || done<0 || total<done || step<progressStep)return false;
            if(step==progressStep && done<progressDone)return false;
            string stage=Convert.ToString(item["stage"]);
            if(!new HashSet<string>{"Checking game files","Files checked","Preparing included tools","Preparing game files","Preparing support files","Preparing your game","Building your game","Finishing game setup","Ready to play"}.Contains(stage))return false;
            if(step!=progressStep)Activity(stage+".");
            progressStep=step;progressDone=done;progressTotal=total;hasProgress=true;
            Get<TextBlock>("ProgressText").Text="Step "+step+" of "+steps+(total>0?"  ·  "+done+" / "+total+" completed":"  ·  Please wait");
            SetStatus(stage,false);UpdateBusyMotion();return true;
        } catch(Exception) { return false; }
    }
    static void CloseReport() {
        lock(reportLock){if(report!=null){report.Dispose();report=null;}}
    }
    static void SetStatus(string text, bool error) {
        status.Text=text;
        status.ToolTip=text;
        Get<Border>("StatusBox").BorderBrush=(Brush)window.Resources[error?"Error":"Edge"];
        status.Foreground=(Brush)window.Resources[error?"Error":"Ink"];
        var marker=Get<Border>("StatusMarker");marker.Background=(Brush)window.Resources[error?"Error":"BuilderYellow"];
        Animate(status,UIElement.OpacityProperty,.72,1,timeline["interaction.status"],0);
        var scale=(ScaleTransform)marker.RenderTransform;
        Animate(scale,ScaleTransform.ScaleXProperty,1.18,1,timeline["interaction.status"],0);
        Animate(scale,ScaleTransform.ScaleYProperty,1.18,1,timeline["interaction.status"],0);
    }
    static void Log(string line) {
        if(line==null)return;
        lock(reportLock){try {if(report!=null)report.WriteLine(line);} catch(IOException) { /* Keep progress visible if the report drive fills up. */ }}
        if(!line.StartsWith("MMR_PROGRESS ") && !line.StartsWith("MMR_MESSAGE ") && !line.StartsWith("Setup failed:"))return;
        window.Dispatcher.BeginInvoke(new Action(delegate {
            if(line.StartsWith("MMR_PROGRESS ")){ApplyProgress(line.Substring(13));return;}
            if(line.StartsWith("MMR_MESSAGE ")){Activity(line.Substring(12));return;}
            failure=line.Substring("Setup failed:".Length).Trim();Activity(failure);
        }));
    }
    static void Start(string action) {
        if(busy)return;
        var fields=new[]{dump};var labels=new[]{"Game folder"};
        for(int i=0;i<fields.Length;i++) {
            bool valid=Directory.Exists(fields[i].Text);
            fields[i].BorderBrush=(Brush)window.Resources[valid?"Edge":"Error"];
            if(!valid){SetStatus(labels[i]+": choose an existing folder before continuing.",true);fields[i].Focus();return;}
        }
        try {
        Directory.CreateDirectory(data);
        File.WriteAllLines(Path.Combine(data,"folders.txt"),new[]{dump.Text},Encoding.UTF8);
        details.Clear(); failure=null; ResetProgress(); SetBusy(true);
        report=new StreamWriter(Path.Combine(data,"preparation-report.txt"),false,new UTF8Encoding(false)){AutoFlush=true};
        Get<Expander>("LogPanel").IsExpanded=action!="validate";
        SetStatus(action=="validate"?"Checking your files...":action=="prepare"?"Preparing your game. Keep this window open.":"Starting your game. The first launch may need to prepare it.",false);
            string helper=Path.Combine(root,"portable","python","python.exe");
            if(!File.Exists(helper))throw new FileNotFoundException("The included setup helper is missing. Extract the full release before starting.",helper);
            var info = new ProcessStartInfo(helper, Quote(Path.Combine(root,"setup.py"))+" "+action+
                " --dump "+Quote(dump.Text));
            info.WorkingDirectory=root; info.UseShellExecute=false; info.CreateNoWindow=true;
            info.RedirectStandardOutput=true; info.RedirectStandardError=true;
            info.StandardOutputEncoding=Encoding.UTF8; info.StandardErrorEncoding=Encoding.UTF8;
            info.EnvironmentVariables["PYTHONUTF8"]="1";
            info.EnvironmentVariables["PYTHONDONTWRITEBYTECODE"]="1";
            var p=new Process(); p.StartInfo=info;
            p.OutputDataReceived+=(s,e)=>Log(e.Data); p.ErrorDataReceived+=(s,e)=>Log(e.Data);
            p.Start(); p.BeginOutputReadLine(); p.BeginErrorReadLine();
            System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                p.WaitForExit(); int code=p.ExitCode; p.Dispose();CloseReport();
                window.Dispatcher.BeginInvoke(new Action(delegate {
                    SetBusy(false);
                    if(code!=0)progress.Foreground=(Brush)window.Resources["Error"];
                    if(code!=0){Get<Expander>("LogPanel").IsExpanded=true;details.BringIntoView();}
                    SetStatus(code!=0?(failure??"Preparation stopped. Open the saved report for details."):
                        action=="validate"?"Files checked. Choose Prepare game, or Play to prepare and start.":
                        action=="prepare"?"Your game is ready. Choose Play to start.":"Game closed. Your saves are in your Windows profile.",code!=0);
                }));
            });
        } catch(Exception error) {
            lock(reportLock){if(report!=null)report.WriteLine(error.ToString());}
            CloseReport();SetBusy(false);Activity("Preparation could not start.");
            Get<Expander>("LogPanel").IsExpanded=true;
            SetStatus("Preparation could not start. Extract the complete launcher package and try again.",true);
        }
    }
    [STAThread] static int Main(string[] args) {
        try {
            timeline=new JavaScriptSerializer().Deserialize<Dictionary<string,double>>(File.ReadAllText(Path.Combine(root,"timeline.json")));
            rendering=args.Length>0 && args[0].StartsWith("--render");
            motionEnabled=(rendering || SystemParameters.ClientAreaAnimation) && Environment.GetEnvironmentVariable("MM2_REDUCE_MOTION")!="1";
            using(var input=File.OpenRead(Path.Combine(root,"Launcher.xaml"))) window=(Window)XamlReader.Load(input);
            window.Title="MushroomMakerRecomp";
            window.Icon=BitmapFrame.Create(new Uri(Path.Combine(root,"assets","branding","mushroom-maker-recomp.ico")));
            dump=Get<TextBox>("Dump");
            details=Get<TextBox>("Details");status=Get<TextBlock>("Status");progress=Get<ProgressBar>("Progress");
            check=Get<Button>("Check");prepare=Get<Button>("Prepare");play=Get<Button>("Play");
            HookMotion();
            window.Activated+=(s,e)=>UpdateBusyMotion();window.Deactivated+=(s,e)=>UpdateBusyMotion();
            var settings=Path.Combine(data,"folders.txt");
            if(File.Exists(settings)) { var paths=File.ReadAllLines(settings,Encoding.UTF8); if(paths.Length>=1)dump.Text=paths[0]; }
            Get<Button>("BrowseDump").Click+=(s,e)=>Browse(dump);
            Get<Button>("OpenReport").Click+=(s,e)=>{
                string path=Path.Combine(data,"preparation-report.txt");
                if(File.Exists(path))Process.Start(new ProcessStartInfo(path){UseShellExecute=true});
                else SetStatus("The report will be available after you check or prepare your files.",false);
            };
            check.Click+=(s,e)=>Start("validate");prepare.Click+=(s,e)=>Start("prepare");play.Click+=(s,e)=>Start("play");
            window.Closing+=(s,e)=>{ if(busy){e.Cancel=true;SetStatus("Preparation or the game is still running. Wait for preparation to finish, or close the game first.",false);} };
            if(args.Length>0 && args[0]=="--selftest") {
                if(Quote(@"C:\a b\")!="\"C:\\a b\\\\\"")return 2;
                dump.Text="";
                Start("validate"); if(busy || !status.Text.Contains("Game folder"))return 3;
                SetBusy(true); if(check.IsEnabled)return 4;
                if(!ApplyProgress("{\"stage\":\"Building your game\",\"completed\":10,\"total\":40,\"step\":6,\"steps\":8}") || progress.Value!=25 || progress.IsIndeterminate)return 7;
                UpdateBusyMotion();if(progress.Value!=25)return 8;
                if(ApplyProgress("{\"stage\":\"Preparing your game\",\"completed\":1,\"total\":1,\"step\":5,\"steps\":8}") || progress.Value!=25)return 9;
                if(ApplyProgress("{\"stage\":\"Building your game\",\"completed\":-1,\"total\":40,\"step\":6,\"steps\":8}"))return 10;
                SetBusy(false); if(!play.IsEnabled)return 5;
                foreach(var size in new[]{new Size(1120,740),new Size(980,680),new Size(800,540),new Size(720,480)})CheckLayout(size.Width,size.Height);
                EntryPose(timeline["review.settled"]);
                foreach(string name in panels)if(Get<FrameworkElement>(name).Opacity!=1 || ((TranslateTransform)Get<FrameworkElement>(name).RenderTransform).Y!=0)return 6;
                return 0;
            }
            if(args.Length>=2 && (args[0]=="--render" || args[0]=="--render-at" || args[0]=="--render-error" || args[0]=="--render-busy")) {
                if(args[0]=="--render-error"){dump.Text="";Start("validate");}
                if(args[0]=="--render-busy"){details.Clear();SetBusy(true);ApplyProgress("{\"stage\":\"Building your game\",\"completed\":23,\"total\":41,\"step\":6,\"steps\":8}");}
                window.Width=args.Length>=4?int.Parse(args[2]):1120;window.Height=args.Length>=4?int.Parse(args[3]):740;
                EntryPose(args.Length==5?double.Parse(args[4],System.Globalization.CultureInfo.InvariantCulture):timeline["review.settled"]);
                MeasureLayout(window.Width,window.Height);var visual=(FrameworkElement)window.Content;
                var bitmap=new RenderTargetBitmap((int)window.Width,(int)window.Height,96,96,PixelFormats.Pbgra32);bitmap.Render(visual);
                var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using(var output=File.Create(args[1]))encoder.Save(output);
                return 0;
            }
            window.MaxWidth=Math.Max(1,SystemParameters.WorkArea.Width-32);window.MaxHeight=Math.Max(1,SystemParameters.WorkArea.Height-32);
            window.MinWidth=Math.Min(window.MinWidth,window.MaxWidth);window.MinHeight=Math.Min(window.MinHeight,window.MaxHeight);
            window.SizeToContent=SizeToContent.WidthAndHeight;
            var app=new Application(); app.Run(window);return 0;
        } catch(Exception error) {
            MessageBox.Show(error.Message,"MushroomMakerRecomp",MessageBoxButton.OK,MessageBoxImage.Error);return 1;
        }
    }
}
