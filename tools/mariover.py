"""Bounded HTTPS client for the documented MariOver public API."""
import base64
from http.client import HTTPConnection, HTTPSConnection
import json
import re
from urllib.parse import urlsplit, urlencode

import community

BASE = 'https://tgrcode.com/mm2'
CODE = re.compile(r'[0123456789BCDFGHJKLMNPQRSTVWXY]{9}')
MODES = {'Populares': 'search_popular', 'Novas': 'search_new',
         'Aleatórias': 'search_endless_mode', 'Código da fase': 'level_info',
         'Código do criador': 'get_posted'}
DIFFICULTIES = {'Fácil': 'e', 'Normal': 'n', 'Difícil': 'ex', 'Muito difícil': 'sex'}


def code(value):
    if not isinstance(value, str):
        raise ValueError('Código inválido')
    value = value.replace('-', '').replace(' ', '').upper()
    if not CODE.fullmatch(value):
        raise ValueError('Informe um código válido de 9 caracteres (XXX-XXX-XXX).')
    return value


def request(server, path, maximum=2*1024*1024, binary=False):
    address = urlsplit(server)
    if (address.scheme not in ('http', 'https') or not address.hostname or address.username or
            address.password or address.query or address.fragment or
            (address.scheme == 'http' and address.hostname != '127.0.0.1')):
        raise ValueError('A API exige HTTPS (HTTP apenas em 127.0.0.1 para testes).')
    connection = (HTTPSConnection if address.scheme == 'https' else HTTPConnection)(
        address.hostname, address.port, timeout=20)
    try:
        connection.request('GET', address.path.rstrip('/') + '/' + path,
                           headers={'User-Agent': 'MM2-PC-Port/0.1 (MariOver client)',
                                    'Accept': 'application/octet-stream,image/jpeg' if binary else 'application/json',
                                    'Accept-Encoding': 'identity'})
        response = connection.getresponse()
        if response.status != 200:
            if response.status == 403:
                raise ValueError('MariOver: HTTP 403 — o serviço recusou o acesso. '
                                 'Não é erro do PostgreSQL. Tente mais tarde ou consulte o operador da API.')
            if response.status == 429:
                raise ValueError('MariOver: HTTP 429 — limite de consultas. Aguarde antes de atualizar.')
            raise ValueError(f'MariOver: HTTP {response.status}; fase indisponível ou serviço com erro.')
        body = response.read(maximum + 1)
        if len(body) > maximum:
            raise ValueError('Resposta da API excede o tamanho permitido.')
        if binary:
            return body
        result = json.loads(body)
        if not isinstance(result, dict) or 'error' in result:
            raise ValueError('A API não encontrou a fase/criador ou retornou um erro.')
        return result
    finally:
        connection.close()


def course(item):
    if not isinstance(item, dict) or not isinstance(item.get('uploader'), dict):
        raise ValueError('Metadados de fase inválidos')
    ident = code(item.get('course_id', ''))
    result = {'id': ident, 'title': item.get('name'), 'author': item['uploader'].get('name')}
    for key in ('title', 'author'):
        value = result[key]
        if not isinstance(value, str) or not value.strip() or len(value) > 64 or any(ord(c) < 32 for c in value):
            raise ValueError('Metadados de fase inválidos: ' + key)
    return result


def catalogue(server, mode='Populares', difficulty='Normal', query=''):
    endpoint = MODES[mode]
    if mode in ('Código da fase', 'Código do criador'):
        result = request(server, endpoint + '/' + code(query))
    else:
        parameters = {'count': 20}
        if mode != 'Novas':
            parameters['difficulty'] = DIFFICULTIES[difficulty]
        result = request(server, endpoint + '?' + urlencode(parameters))
    items = [result] if mode == 'Código da fase' else result.get('courses')
    if not isinstance(items, list) or len(items) > 100:
        raise ValueError('Catálogo inválido')
    courses = [course(item) for item in items]
    if len({item['id'] for item in courses}) != len(courses):
        raise ValueError('Catálogo contém códigos duplicados')
    return {'courses': courses, 'total': len(courses)}


def fetch(server, ident):
    from mm2_course_crypto import check_course, thumbnail
    ident = code(ident)
    metadata = course(request(server, 'level_info/' + ident))
    if metadata['id'] != ident:
        raise ValueError('A API retornou outra fase')
    data = request(server, 'level_data/' + ident, maximum=0x5c000, binary=True)
    check_course(data)
    jpeg = request(server, 'level_thumbnail/' + ident, maximum=512*1024, binary=True)
    bundle = {'title': metadata['title'], 'author': metadata['author'], 'files': {
        'data': base64.b64encode(data).decode('ascii'),
        'thumbnail': base64.b64encode(thumbnail(jpeg)).decode('ascii')}}
    community.validate(bundle)
    return bundle
