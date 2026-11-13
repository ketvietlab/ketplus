#pragma once
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

inline bool writeFakeCopilot(const QString& path, const QString& log, const QString& mode = {}) {
    const auto quoted = [](const QString& value) {
        QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(json.mid(1, json.size() - 2));
    };
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QString::fromUtf8(R"PY(#!/usr/bin/env python3
import json, os, sys
log_path = %1
mode = %2
pending = None
with open(log_path, 'a') as output:
    output.write(json.dumps({'pid':os.getpid(),'argv':sys.argv[1:]}) + '\n')

def emit(obj):
    print(json.dumps(obj), flush=True)

def reply(id, obj):
    emit({'jsonrpc':'2.0','id':id,'result':obj})

def error(id, message, data=None):
    obj = {'code':-32000,'message':message}
    if data: obj['data'] = data
    emit({'jsonrpc':'2.0','id':id,'error':obj})

def update(text):
    emit({'jsonrpc':'2.0','method':'session/update','params':{
        'sessionId':'copilot-session','update':{'sessionUpdate':'agent_message_chunk',
        'content':{'type':'text','text':text}}}})

for line in sys.stdin:
    msg = json.loads(line)
    with open(log_path, 'a') as output:
        output.write(json.dumps(msg) + '\n')
    method = msg.get('method')
    id = msg.get('id')
    if method == 'initialize':
        if mode == 'hang': continue
        if mode == 'exit-auth':
            print('Authentication required', file=sys.stderr, flush=True)
            sys.exit(1)
        if mode == 'init-auth':
            error(id, 'Authentication required')
            continue
        if mode == 'init-nested-auth':
            error(id, 'Initialization failed', {'message':'Authentication required'})
            continue
        if mode == 'auth-once' and not os.path.exists(log_path+'.authenticated'):
            open(log_path+'.authenticated','w').close()
            error(id, 'Authentication required')
            continue
        capabilities = {} if mode == 'no-load' else {'loadSession':True}
        reply(id, {'protocolVersion':2 if mode == 'unsupported' else 1,'agentCapabilities':capabilities})
    elif method == 'session/new':
        if mode == 'new-auth': error(id, 'Authentication required')
        else: reply(id, {'sessionId':'copilot-session'})
    elif method in ('session/load','session/set_mode'):
        reply(id, {})
    elif method == 'session/prompt':
        text = msg['params']['prompt'][-1]['text']
        pending = id
        if text == 'hold': update('waiting')
        if mode == 'prompt-auth': error(id, 'Authentication required')
        elif mode == 'permission': error(id, 'Permission denied for this tool')
        elif mode == 'network': error(id, 'Network connection failed')
        elif mode == 'usage': error(id, 'Usage limit exceeded')
        elif mode == 'secret': error(id, 'Network failed ghp_fixtureSecret123 Authorization: Bearer fixture-bearer')
        elif text != 'hold':
            update('hello ')
            update('world')
            reply(id, {'stopReason':'end_turn'})
    elif method == 'session/cancel':
        reply(pending, {'stopReason':'cancelled'})
    else:
        reply(id, {})
)PY")
                   .arg(quoted(log), quoted(mode))
                   .toUtf8());
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
