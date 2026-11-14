#pragma once
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

inline bool writeFakeCopilotTools(const QString& path, const QString& log) {
    const auto quoted = QJsonDocument(QJsonArray{log}).toJson(QJsonDocument::Compact);
    const QByteArray logString = quoted.mid(1, quoted.size() - 2);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray("#!/usr/bin/env python3\nlog_path = ") + logString + R"PY(
import json, os, sys
pending = None
session = 'copilot-session'

def emit(obj): print(json.dumps(obj), flush=True)
def reply(id, obj): emit({'jsonrpc':'2.0','id':id,'result':obj})
def update(obj): emit({'jsonrpc':'2.0','method':'session/update','params':{'sessionId':session,'update':obj}})
for line in sys.stdin:
    msg = json.loads(line)
    with open(log_path,'a') as output: output.write(json.dumps(msg)+'\n')
    method = msg.get('method')
    id = msg.get('id')
    if method == 'initialize':
        reply(id, {'protocolVersion':1,'agentCapabilities':{'loadSession':True,'promptCapabilities':{'image':True}}})
    elif method == 'session/new':
        if os.path.exists(log_path+'.newfail'):
            emit({'jsonrpc':'2.0','id':id,'error':{'code':-32000,'message':'MCP setup failed'}})
        else: reply(id, {'sessionId':session})
    elif method == 'session/load': reply(id, {})
    elif method == 'session/prompt':
        pending = id
        text = next(b['text'] for b in reversed(msg['params']['prompt']) if b.get('type') == 'text')
        if text == 'fail':
            emit({'jsonrpc':'2.0','id':id,'error':{'code':-32000,'message':'Network failed'}})
        elif text == 'complete': reply(id, {'stopReason':'end_turn'})
        else:
            options = [{'optionId':'allow-custom','kind':'allow_once'}, {'optionId':'always-custom','kind':'allow_always'}, {'optionId':'deny-custom','kind':'reject_once'}]
            if text == 'no-options': options = []
            if text == 'always-only': options = [options[1]]
            identity = {'providerIdentifier':'ketplus_browser','toolName':'browser_capture'}
            emit({'jsonrpc':'2.0','id':77,'method':'session/request_permission','params':{'sessionId':'unknown' if text == 'foreign' else session,'options':options,'toolCall':{'toolCallId':'capture','kind':'other','title':'mcp__ketplus_browser__browser_capture','rawInput':identity}}})
    elif method == 'session/cancel': reply(pending, {'stopReason':'cancelled'})
    elif method is None and id == 77:
        if msg.get('result',{}).get('outcome',{}).get('optionId') == 'allow-custom':
            update({'sessionUpdate':'tool_call','toolCallId':'capture','title':'browser_capture','kind':'other','status':'in_progress'})
            update({'sessionUpdate':'tool_call_update','toolCallId':'capture','status':'completed','content':[{'type':'content','content':{'type':'image','mimeType':'image/png','data':'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aWQ0AAAAASUVORK5CYII='}}]})
        reply(pending, {'stopReason':'end_turn'})
    else: reply(id, {})
)PY");
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
