"""Headless MariOver worker for the game's original Course World catalogue."""
import argparse
import base64
import json
from itertools import zip_longest
import os
import sqlite3
from contextlib import closing
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import struct
import time

import mariover

HEADER = struct.Struct('<4sIII512s')
RECORD = struct.Struct('<QQ20s66s22s202s12IQ10sB88s4H')


def wide(value, size):
    if not isinstance(value, str):
        raise ValueError("Texto inválido na resposta MariOver")
    return (value.encode('utf-16-le')[:size-2].decode('utf-16-le', errors='ignore')
            .encode('utf-16-le') + b'\0\0').ljust(size, b'\0')


def number(value, maximum=0xffffffff):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('Campo numérico inválido na resposta MariOver')
    return value


def mii(user):
    value = user.get('mii_data')
    if value is None:
        return bytes(88)
    result = base64.b64decode(value, validate=True)
    if len(result) != 88:
        raise ValueError('Dados Mii inválidos')
    return result


def pack(result, version=4):
    if version not in (3, 4):
        raise ValueError('Versão de catálogo inválida')
    items = result.get('courses')
    if not isinstance(items, list) or len(items) > 100:
        raise ValueError('Catálogo inválido')
    records = []
    seen = set()
    for item in items:
        metadata = mariover.course(item)
        if metadata['id'] in seen:
            raise ValueError('Fase duplicada')
        seen.add(metadata['id'])
        records.append(RECORD.pack(number(item['data_id'], 0xffffffffffffffff),
            number(item['uploader']['pid'], 0xffffffffffffffff), wide(metadata['id'], 20),
            wide(metadata['title'], 66), wide(metadata['author'], 22),
            wide(item.get('description', ''), 202),
            number(item['game_style'], 4), number(item['difficulty'], 3),
            number(item['theme'], 9), number(item.get('likes', 0)),
            number(item.get('plays', 0)), number(item.get('clears', 0)),
            number(item.get('attempts', 0)), number(item.get('tag1', 0), 15),
            number(item.get('tag2', 0), 15), number(item.get('world_record', 0)),
            number(item.get('weekly_likes', 0)), number(item.get('weekly_plays', 0)),
            number(item.get('uploaded', 0), 0x7fffffffffffffff),
            mariover.code(item['uploader'].get('code', '000000000')).encode('ascii') + b'\0',
            number(item['uploader'].get('region', 0), 3), mii(item['uploader']),
            *(number(item['uploader'].get(key, 0), 65535) for key in ('pose', 'hat', 'shirt', 'pants'))))
    if version == 3:
        records = [record[:382] for record in records]
    page_info = ('M2PAGE %d %d' % (result['offset'], bool(result.get('has_more')))).encode('ascii') if 'offset' in result else b''
    return HEADER.pack(b'M2CW', version, len(records), 0, page_info) + b''.join(records)


def catalogue(mode, difficulty, count=20):
    endpoint = {'popular': 'search_popular', 'new': 'search_new'}[mode]
    if mode == 'new':
        return mariover.request(mariover.BASE, endpoint + '?count=' + str(count))
    if difficulty != 'all':
        return mariover.request(mariover.BASE, endpoint + '?count=' + str(count) + '&difficulty=' + difficulty)
    # MariOver requires one difficulty per popular request; merge the four samples.
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda d: mariover.request(mariover.BASE,
            endpoint + '?count=' + str((count+3)//4) + '&difficulty=' + d), ('e', 'n', 'ex', 'sex')))
    courses = []
    seen = set()
    if any(not isinstance(result.get('courses'),list) for result in results):
        raise ValueError('Catálogo inválido')
    for row in zip_longest(*(result['courses'] for result in results)):
        for item in row:
            if item is None:continue
            ident = mariover.course(item)['id']
            if ident not in seen:
                courses.append(item)
                seen.add(ident)
    return {'courses': courses[:count]}


def cache_thumbnail(item, cache):
    from mm2_course_crypto import thumbnail, decrypt, tables
    ident = mariover.code(item['course_id'])
    path = cache / f"{number(item['data_id'], 0xffffffffffffffff)}.btl"
    try:
        jpeg_path = path.with_suffix('.jpg')
        if jpeg_path.is_file() and jpeg_path.stat().st_size == 0x1c000:
            with jpeg_path.open('rb') as existing:
                if existing.read(2) == b'\xff\xd8':
                    return True
        if path.is_file() and path.stat().st_size == 0x1c000:
            data = path.read_bytes()
            plain = decrypt(data, tables.btl)
        else:
            time.sleep(0.6)
            jpeg = mariover.request(mariover.BASE, 'level_thumbnail/' + ident, maximum=512*1024, binary=True)
            data = thumbnail(jpeg)
            plain = decrypt(data, tables.btl)
            temporary = path.with_suffix('.partial.'+str(os.getpid()))
            temporary.write_bytes(data)
            temporary.replace(path)
        temporary = path.with_suffix('.jpg.partial.'+str(os.getpid()))
        temporary.write_bytes(plain.ljust(0x1c000, b'\0'))
        temporary.replace(path.with_suffix('.jpg'))
        return True
    except Exception as exc:
        print(f'Miniatura {ident}: {exc}', flush=True)
        return False


def filter_tags(result, tag1=255, tag2=255):
    if any(type(tag) is not int or tag not in (*range(16), 255) for tag in (tag1, tag2)):
        raise ValueError('Tag de busca inválida')
    # shortcut: MariOver exposes sampled catalogues, so filtering cannot cover every stored course.
    return {'courses': [item for item in result['courses']
                        if all(tag == 255 or tag in (item.get('tag1', 0), item.get('tag2', 0))
                               for tag in (tag1, tag2))]}


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.partial.'+str(os.getpid()))
    temporary.write_bytes(data)
    temporary.replace(path)


def catalogue_page(mode, difficulty, cache, offset=0, count=20, tag1=255, tag2=255, tab=0):
    if type(offset) is not int or not 0 <= offset <= 0x7fffffff-101 or type(count) is not int or not 1 <= count <= 100 or offset % count:
        raise ValueError('Invalid catalogue page')
    cache.mkdir(parents=True,exist_ok=True)
    query=f'{tab}-{mode}-{difficulty}-{tag1}-{tag2}-{count}'
    # Keep page history on disk: browsing thousands of courses must not grow a single JSON response.
    with closing(sqlite3.connect(cache/'catalogues.sqlite3',timeout=25)) as db, db:
        db.execute('PRAGMA foreign_keys=ON')
        db.executescript("""
            CREATE TABLE IF NOT EXISTS queries (id TEXT PRIMARY KEY, updated REAL NOT NULL, pending TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS pages (query TEXT REFERENCES queries(id) ON DELETE CASCADE,
                offset INTEGER NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(query,offset));
            CREATE TABLE IF NOT EXISTS seen (query TEXT REFERENCES queries(id) ON DELETE CASCADE,
                code TEXT NOT NULL, PRIMARY KEY(query,code));
        """)
        db.execute('BEGIN IMMEDIATE')
        row=db.execute('SELECT updated,pending FROM queries WHERE id=?',(query,)).fetchone()
        now=time.time()
        # Refresh after an idle session, never expire earlier pages during active navigation.
        if row and not 0 <= now-row[0] < 600:
            db.execute('DELETE FROM queries WHERE id=?',(query,));row=None
        if not row:
            db.execute('INSERT INTO queries VALUES (?,?,?)',(query,now,'[]'))
        db.execute('UPDATE queries SET updated=? WHERE id=?',(now,query))
        saved=db.execute('SELECT payload FROM pages WHERE query=? AND offset=?',(query,offset)).fetchone()
        if saved:
            page=json.loads(saved[0]);pack(page)
            return page
        pending=json.loads(row[1]) if row else []
        pack({'courses':pending})
        # HTTP must not hold the cache writer lock while the user changes tabs.
        db.commit()
        for attempt in range(3):
            if len(pending)>=count:break
            sample=filter_tags(catalogue(mode,difficulty,100),tag1,tag2);pack(sample)
            known={item['course_id'] for item in pending}
            for item in sample['courses']:
                code=item['course_id']
                if code not in known and not db.execute('SELECT 1 FROM seen WHERE query=? AND code=?',(query,code)).fetchone():
                    pending.append(item);known.add(code)
            if not sample['courses']:break
        db.execute('BEGIN IMMEDIATE')
        saved=db.execute('SELECT payload FROM pages WHERE query=? AND offset=?',(query,offset)).fetchone()
        if saved:
            page=json.loads(saved[0]);pack(page)
            return page
        latest=db.execute('SELECT pending FROM queries WHERE id=?',(query,)).fetchone()
        candidates=(json.loads(latest[0]) if latest else [])+pending
        pending=[];known=set()
        for item in candidates:
            code=item['course_id']
            if code not in known and not db.execute('SELECT 1 FROM seen WHERE query=? AND code=?',(query,code)).fetchone():
                pending.append(item);known.add(code)
        if not pending:
            # Repeated random samples do not prove that the API's entire catalogue is exhausted.
            raise ValueError('No new courses in this sample. Please try again shortly.')
        courses,reserve=pending[:count],pending[count:]
        page={'courses':courses,'offset':offset,'has_more':True}
        db.execute('UPDATE queries SET pending=? WHERE id=?',(json.dumps(reserve,ensure_ascii=True),query))
        db.execute('INSERT INTO pages VALUES (?,?,?)',(query,offset,json.dumps(page,ensure_ascii=True)))
        db.executemany('INSERT INTO seen VALUES (?,?)',((query,item['course_id']) for item in courses))
    metadata=cache/'metadata';metadata.mkdir(exist_ok=True)
    for item in courses:
        atomic_write(metadata/(str(item['data_id'])+'.json'),json.dumps(item,ensure_ascii=True).encode('utf-8'))
    return page


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('popular', 'new'), default='popular')
    parser.add_argument('--difficulty', choices=('e', 'n', 'ex', 'sex', 'all'), default='all')
    parser.add_argument('--version', type=int, choices=(3, 4), default=3)
    parser.add_argument('--tag1', type=int, choices=(*range(16), 255), default=255)
    parser.add_argument('--tag2', type=int, choices=(*range(16), 255), default=255)
    parser.add_argument('--offset',type=int,default=0)
    parser.add_argument('--count',type=int,default=20)
    parser.add_argument('--tab',type=int,choices=range(6),default=0)
    args = parser.parse_args()
    try:
        cache = Path(__file__).resolve().parents[1] / 'build/cache/mariover'
        cache.mkdir(parents=True, exist_ok=True)
        result = catalogue_page(args.mode,args.difficulty,cache,args.offset,args.count,args.tag1,args.tag2,args.tab)
        # Publish the list first. The native UI can open while pictures are still arriving.
        atomic_write(args.output,pack(result,args.version))
    except Exception as exc:
        atomic_write(args.output,HEADER.pack(b'M2CW',args.version,0,1,str(exc).encode('utf-8')[:511]))
        return 0
    deadline = time.monotonic()+30
    for item in result['courses']:
        if time.monotonic() >= deadline or not cache_thumbnail(item,cache):
            break
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
