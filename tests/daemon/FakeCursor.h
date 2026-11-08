#pragma once

#include <QFile>
#include <QString>

inline bool writeFakeCursor(const QString& path, const QString& logPath) {
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly))
        return false;
    QString escapedLog = logPath;
    escapedLog.replace('\\', "\\\\");
    escapedLog.replace('\'', "\\'");
    const QString source = QString::fromUtf8(R"PY(#!/usr/bin/env python3
import json, os, sys, threading

# ADR 0015 version probe: answer before logging so launch counts stay unchanged.
if sys.argv[1:] == ['--version']:
    print(os.environ.get('KETPLUS_FAKE_CURSOR_VERSION', '2026.09.18-test'))
    sys.exit(0)

log_path = '%1'
turns = 0
lock = threading.Lock()
sessions = {}

def emit(value):
    with lock:
        print(json.dumps(value), flush=True)

def log(value):
    with open(log_path, 'a', encoding='utf-8') as output:
        output.write(value + '\n')

log('args:' + json.dumps(sys.argv[1:]))

current_model = 'auto'
argv = sys.argv[1:]
if '--model' in argv:
    index = argv.index('--model')
    if index + 1 < len(argv):
        current_model = argv[index + 1]

def acp_model_id(cli):
    name = str(cli or 'auto')
    if name in ('auto', 'default', 'auto-smart', ''):
        return 'default'
    if name.endswith('-fast'):
        name = name[:-5]
    for suffix in ('-extra-high', '-minimal', '-medium', '-xhigh', '-none', '-high', '-low', '-max'):
        if name.endswith(suffix) and name != suffix[1:]:
            name = name[:-len(suffix)]
            break
    if name.startswith('cursor-'):
        name = name[7:]
    return name

def config_options():
    model = acp_model_id(current_model)
    options = [
        {'id': 'mode', 'name': 'Mode', 'category': 'mode', 'type': 'select',
         'currentValue': 'agent', 'options': [{'value': 'agent', 'name': 'Agent'}]},
        {'id': 'model', 'name': 'Model', 'category': 'model', 'type': 'select',
         'currentValue': model, 'options': [
             {'value': 'default', 'name': 'Auto'},
             {'value': 'composer-2.5', 'name': 'Composer 2.5'},
             {'value': 'composer-2.5[fast=false]', 'name': 'Composer 2.5'},
             {'value': 'grok-4.6', 'name': 'Cursor Grok 4.6'}]},
    ]
    if model == 'composer-2.5':
        options.append({'id': 'fast', 'name': 'Fast', 'category': 'model_config',
                        'type': 'select', 'currentValue': 'true',
                        'options': [{'value': 'false', 'name': 'Off'},
                                    {'value': 'true', 'name': 'Fast'}]})
    if model == 'grok-4.6':
        options.append({'id': 'effort', 'name': 'Effort', 'category': 'thought_level',
                        'type': 'select', 'currentValue': 'high',
                        'options': [{'value': 'low', 'name': 'Low'},
                                    {'value': 'medium', 'name': 'Medium'},
                                    {'value': 'high', 'name': 'High'},
                                    {'value': 'xhigh', 'name': 'Extra High'}]})
        options.append({'id': 'fast', 'name': 'Fast', 'category': 'model_config',
                        'type': 'select', 'currentValue': 'true',
                        'options': [{'value': 'false', 'name': 'Off'},
                                    {'value': 'true', 'name': 'Fast'}]})
    return options

if 'models' in sys.argv[1:] or '--list-models' in sys.argv[1:]:
    print('Available models')
    print('')
    print('auto - Auto (default)')
    print('composer-2.5 - Composer 2.5')
    print('cursor-grok-4.6-low - Cursor Grok 4.6')
    print('cursor-grok-4.6 - Cursor Grok 4.6')
    print('cursor-grok-4.6-high - Cursor Grok 4.6')
    print('cursor-grok-4.6-high-fast - Cursor Grok 4.6 Fast')
    sys.exit(0)

def finish(rpc_id, session_id, text='finished'):
    emit({'jsonrpc': '2.0', 'method': 'session/update', 'params': {
        'sessionId': session_id,
        'update': {'sessionUpdate': 'agent_message_chunk',
                   'content': {'type': 'text', 'text': text}}}})
    emit({'jsonrpc': '2.0', 'method': 'session/update', 'params': {
        'sessionId': session_id,
        'update': {'sessionUpdate': 'usage_update',
                   'used': 12000, 'size': 200000}}})
    emit({'jsonrpc': '2.0', 'id': rpc_id, 'result': {'stopReason': 'end_turn'}})

waiting = {}
waiting_questions = {}
hanging = {}

for line in sys.stdin:
    message = json.loads(line)
    method = message.get('method')
    message_id = message.get('id')
    params = message.get('params') or {}
    if method is None and message_id in waiting:
        log('permission:' + json.dumps(message.get('result') or message.get('error')))
        prompt_id, session_id = waiting.pop(message_id)
        finish(prompt_id, session_id)
        continue
    if method is None and message_id in waiting_questions:
        log('question:' + json.dumps(message.get('result') or message.get('error'), sort_keys=True))
        prompt_id, session_id = waiting_questions.pop(message_id)
        finish(prompt_id, session_id)
        continue
    if method == 'initialize':
        log('initialize:' + json.dumps(params.get('clientInfo') or {}))
        result = {'protocolVersion': 1,
                  'agentCapabilities': {'loadSession': True, 'promptCapabilities': {'image': True}},
                  'authMethods': [{'id': 'cursor_login', 'name': 'Cursor login'}]}
        capability_mode = os.environ.get('KETPLUS_FAKE_CURSOR_LOAD_SESSION')
        if capability_mode == '0':
            result['agentCapabilities'] = {'loadSession': False}
        elif capability_mode == 'missing':
            result['agentCapabilities'] = {}
        elif capability_mode == 'absent':
            del result['agentCapabilities']
        elif capability_mode == 'wrong-type':
            result['agentCapabilities'] = {'loadSession': 'true'}
        emit({'jsonrpc': '2.0', 'id': message_id, 'result': result})
    elif method == 'authenticate':
        log('authenticate:' + json.dumps(params))
        emit({'jsonrpc': '2.0', 'id': message_id, 'error': {
            'code': -32600, 'message': 'authenticate should not be called'}})
    elif method == 'session/new':
        log('browser:' + json.dumps(params))
        cwd = params.get('cwd') or ''
        log('session/new:' + cwd)
        if os.path.basename(cwd.rstrip('/')) == 'needs-login':
            emit({'jsonrpc': '2.0', 'id': message_id, 'error': {
                'code': -32000, 'message': 'Not authenticated'}})
            continue
        turns += 1
        session_id = 'acp-session-' + str(turns)
        sessions[session_id] = cwd
        emit({'jsonrpc': '2.0', 'id': message_id, 'result': {
            'sessionId': session_id, 'configOptions': config_options()}})
    elif method == 'session/load':
        log('browser:' + json.dumps(params))
        session_id = params.get('sessionId')
        log('session/load:' + str(session_id))
        if session_id in sessions:
            emit({'jsonrpc': '2.0', 'id': message_id, 'result': {
                'configOptions': config_options()}})
        else:
            emit({'jsonrpc': '2.0', 'id': message_id, 'error': {
                'code': -32000, 'message': 'Unknown session'}})
    elif method == 'session/set_mode':
        log('set_mode:' + str(params.get('modeId')))
        emit({'jsonrpc': '2.0', 'id': message_id, 'result': {}})
    elif method == 'session/set_config_option':
        log('set_config_option:' + str(params.get('configId')) + '=' + str(params.get('value')))
        if params.get('configId') == 'model' and params.get('value'):
            current_model = str(params.get('value'))
        emit({'jsonrpc': '2.0', 'id': message_id, 'result': {'configOptions': config_options()}})
    elif method == 'session/prompt':
        session_id = params.get('sessionId')
        blocks = params.get('prompt') or []
        policy = blocks[0].get('text', '') if blocks else ''
        if policy.startswith('[KetPlus browser policy]'):
            log('policy:' + json.dumps({'instructions': policy, 'blocks': blocks[1:]}))
            blocks = blocks[1:]
        parts = []
        for item in blocks:
            if item.get('type') == 'text':
                parts.append(item.get('text') or '')
        prompt = '\n'.join(parts)
        log('prompt:' + session_id + ':' + prompt)
        prompt = prompt.split('\n\n<ketplus_response_preferences>')[0]
        if prompt == 'create-plan':
            plan_id = 'plan-' + str(message_id)
            waiting[plan_id] = (message_id, session_id)
            emit({'jsonrpc':'2.0','method':'session/update','params':{'sessionId':session_id,
                'update':{'sessionUpdate':'tool_call','toolCallId':'plan-tool','kind':'think',
                          'title':'Plan','status':'in_progress'}}})
            emit({'jsonrpc':'2.0','id':plan_id,'method':'cursor/create_plan','params':{
                'toolCallId':'plan-tool','name':'Implement feature','overview':'Three steps',
                'plan':'Review, implement, verify','todos':[]}})
        elif prompt in ('needs-approval', 'needs-custom-approval', 'needs-browser-approval'):
            perm_id = 'perm-' + str(message_id)
            waiting[perm_id] = (message_id, session_id)
            emit({'jsonrpc': '2.0', 'id': perm_id, 'method': 'session/request_permission',
                  'params': {'sessionId': session_id, 'toolCall': {
                      'toolCallId': 'call-1', 'title': 'Bash',
                      'rawInput': ({'providerIdentifier': 'ketplus_browser', 'toolName': 'browser_open', 'args': {}}
                                   if prompt == 'needs-browser-approval' else {'command': 'ls -la'})},
                      'options': [
                          {'optionId': 'custom-allow' if prompt == 'needs-custom-approval' else 'allow-once', 'kind': 'allow_once', 'name': 'Allow once'},
                          {'optionId': 'custom-reject' if prompt == 'needs-custom-approval' else 'reject-once', 'kind': 'reject_once', 'name': 'Reject'}]}})
        elif prompt == 'ask-question':
            question_id = 'question-' + str(message_id)
            waiting_questions[question_id] = (message_id, session_id)
            emit({'jsonrpc': '2.0', 'id': question_id, 'method': 'cursor/ask_question',
                  'params': {'sessionId': session_id, 'toolCallId': 'ask-1', 'title': 'Need input',
                             'questions': [{'id': 'scope', 'prompt': 'Which scope?',
                                            'options': [{'id': 'small', 'label': 'Small'},
                                                        {'id': 'full', 'label': 'Full'}]}]}})
        elif prompt == 'hang':
            hanging[session_id] = message_id
        elif prompt == 'slow-start':
            def late(rpc_id=message_id, sid=session_id):
                finish(rpc_id, sid)
            threading.Timer(0.2, late).start()
        elif prompt == 'always-upgrade' or (
                prompt == 'upgrade-plan' and current_model in ('auto', 'default', 'auto-smart', '')):
            emit({'jsonrpc': '2.0', 'method': 'session/update', 'params': {
                'sessionId': session_id,
                'update': {'sessionUpdate': 'agent_message_chunk',
                           'content': {'type': 'text',
                                       'text': 'Upgrade your plan to continue'}}}})
            emit({'jsonrpc': '2.0', 'id': message_id, 'result': {'stopReason': 'end_turn'}})
        else:
            finish(message_id, session_id)
    elif method == 'session/cancel':
        session_id = params.get('sessionId')
        log('cancel:' + str(session_id))
        emit({'jsonrpc': '2.0', 'id': message_id, 'result': {}})
        if session_id in hanging:
            prompt_id = hanging.pop(session_id)
            emit({'jsonrpc': '2.0', 'id': prompt_id, 'result': {'stopReason': 'cancelled'}})
    elif message_id is not None:
        emit({'jsonrpc': '2.0', 'id': message_id, 'error': {'message': 'Unknown method'}})
)PY")
                               .arg(escapedLog);
    const QByteArray encoded = source.toUtf8();
    if (script.write(encoded) != encoded.size())
        return false;
    script.close();
    return script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
}
