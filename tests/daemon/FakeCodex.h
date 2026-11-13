#pragma once

#include <QFile>
#include <QString>

inline bool writeFakeCodex(const QString& path, const QString& logPath) {
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly)) {
        return false;
    }
    QString escapedLog = logPath;
    escapedLog.replace('\\', "\\\\");
    escapedLog.replace('\'', "\\'");
    const QString source = QString::fromUtf8(R"PY(#!/usr/bin/env python3
import json, os, sys, threading, time

# ADR 0015 version probe: answer before logging so launch counts stay unchanged.
if sys.argv[1:] == ['--version']:
    print('codex-cli ' + os.environ.get('KETPLUS_FAKE_CODEX_VERSION', '0.154.0'))
    sys.exit(0)

log_path = '%1'
lock = threading.Lock()
threads = 0
turns = 0
approvals = {}
names = {}
previews = {}
limit_used = False

def emit(value):
    with lock:
        print(json.dumps(value), flush=True)

def log(value):
    with open(log_path, 'a', encoding='utf-8') as output:
        output.write(value + '\n')

log('args:' + json.dumps(sys.argv[1:]))
default_mode_request_user_input = (
    'features.default_mode_request_user_input=true' in sys.argv[1:]
)

def finish(thread_id, turn_id):
    emit({'method': 'item/agentMessage/delta', 'params': {
        'threadId': thread_id, 'itemId': 'answer-' + turn_id, 'delta': 'finished'}})
    emit({'method': 'turn/completed', 'params': {
        'threadId': thread_id, 'turn': {'id': turn_id, 'status': 'completed'}}})

for line in sys.stdin:
    message = json.loads(line)
    method = message.get('method')
    message_id = message.get('id')
    # ADR 0015: an older app-server answers methods it lacks with JSON-RPC -32601.
    if method in os.environ.get('KETPLUS_FAKE_CODEX_UNSUPPORTED', '').split(','):
        log('unsupported:' + method)
        emit({'id': message_id, 'error': {'code': -32601, 'message': 'Method not found'}})
        continue
    if method == 'initialize':
        emit({'id': message_id, 'result': {}})
    elif method == 'model/list':
        emit({'id': message_id, 'result': {'data': [{
            'model': 'gpt-5.6-luna', 'displayName': 'Luna', 'hidden': False,
            'defaultReasoningEffort': 'medium', 'supportedReasoningEfforts': []}]}})
    elif method == 'account/rateLimits/read':
        emit({'id': message_id, 'result': {'rateLimits': {
            'limitId': 'codex', 'secondary': {'usedPercent': 100 if limit_used else 42,
            'windowDurationMins': 10080, 'resetsAt': int(time.time()) + 3600}}}})
    elif method == 'account/read':
        # #170 readiness: KETPLUS_FAKE_CODEX_SIGNED_OUT reports no account.
        if os.environ.get('KETPLUS_FAKE_CODEX_SIGNED_OUT'):
            emit({'id': message_id, 'result': {'account': None, 'requiresOpenaiAuth': True}})
        else:
            emit({'id': message_id, 'result': {'account': {
                'type': 'chatgpt', 'email': 'dev@example.com', 'planType': 'plus'},
                'requiresOpenaiAuth': True}})
    elif method == 'thread/delete':
        log('delete:' + message['params']['threadId'])
        emit({'id': message_id, 'result': {}})
    elif method == 'thread/start':
        log('setup:' + json.dumps(message['params']))
        threads += 1
        thread_id = 'thread-' + str(threads)
        emit({'id': message_id, 'result': {'thread': {'id': thread_id}}})
    elif method == 'thread/fork':
        log('fork:' + json.dumps(message['params'], sort_keys=True))
        if not message['params'].get('excludeTurns', False):
            emit({'id': message_id, 'error': {'message': 'fork must exclude historical turns'}})
            continue
        thread_id = 'fork-of-' + message['params']['threadId']
        emit({'id': message_id, 'result': {'thread': {
            'id': thread_id, 'forkedFromId': message['params']['threadId']}}})
    elif method == 'thread/resume':
        log('setup:' + json.dumps(message['params']))
        thread_id = message['params']['threadId']
        emit({'id': message_id, 'result': {'thread': {'id': thread_id}}})
    elif method == 'thread/read':
        thread_id = message['params']['threadId']
        emit({'id': message_id, 'result': {'thread': {
            'id': thread_id, 'name': names.get(thread_id),
            'preview': previews.get(thread_id, '')}}})
    elif method == 'thread/name/set':
        thread_id = message['params']['threadId']
        name = message['params']['name']
        names[thread_id] = name
        log('name:' + thread_id + ':' + name)
        emit({'id': message_id, 'result': {}})
        emit({'method': 'thread/name/updated', 'params': {
            'threadId': thread_id, 'threadName': name}})
    elif method == 'turn/start':
        if os.environ.get('KETPLUS_FAKE_CODEX_LOG_INPUT'):
            log('input:' + json.dumps(message['params']['input']))
        log('turn-start:' + json.dumps({'approvalPolicy': message['params'].get('approvalPolicy'),
                                        'sandboxPolicy': message['params'].get('sandboxPolicy')}))
        turns += 1
        thread_id = message['params']['threadId']
        turn_id = 'turn-' + str(turns)
        text = message['params']['input'][0]['text']
        previews.setdefault(thread_id, text)
        log('turn:' + thread_id)
        log('cwd:' + json.dumps(message['params'].get('cwd')))
        if text == 'slow-start':
            # The response arrives well before turn/started (reload race window).
            emit({'id': message_id, 'result': {'turn': {'id': turn_id}}})
            def late_start(thread_id=thread_id, turn_id=turn_id):
                emit({'method': 'turn/started', 'params': {
                    'threadId': thread_id, 'turn': {'id': turn_id}}})
                finish(thread_id, turn_id)
            threading.Timer(0.6, late_start).start()
            continue
        if text == 'fail-start':
            # turn/start fails after a moment, without a turn/started notification.
            def refuse(message_id=message_id):
                emit({'id': message_id, 'error': {'code': -32000, 'message': 'start failed'}})
            threading.Timer(0.3, refuse).start()
            continue
        if text == 'limit-final':
            # #170: the weekly limit, as Codex reports it; the rate limits read then shows it.
            limit_used = True
            emit({'id': message_id, 'result': {'turn': {'id': turn_id}}})
            emit({'method': 'turn/started', 'params': {
                'threadId': thread_id, 'turn': {'id': turn_id}}})
            emit({'method': 'error', 'params': {
                'threadId': thread_id, 'turnId': turn_id, 'willRetry': False,
                'error': {'message': "You've hit your weekly limit", 'codexErrorInfo': 'usageLimitExceeded'}}})
            emit({'method': 'turn/completed', 'params': {
                'threadId': thread_id, 'turn': {'id': turn_id, 'status': 'failed'}}})
            continue
        if text == 'error-final':
            # A turn that ends with a final error and never sends turn/completed.
            emit({'id': message_id, 'result': {'turn': {'id': turn_id}}})
            emit({'method': 'turn/started', 'params': {
                'threadId': thread_id, 'turn': {'id': turn_id}}})
            emit({'method': 'error', 'params': {
                'threadId': thread_id, 'turnId': turn_id, 'willRetry': False,
                'error': {'message': 'stream failed'}}})
            continue
        emit({'id': message_id, 'result': {'turn': {'id': turn_id}}})
        emit({'method': 'turn/started', 'params': {
            'threadId': thread_id, 'turn': {'id': turn_id}}})
        if text == 'hang':
            continue
        if text == 'oversized':
            # Messages over the daemon's 16 MiB line limit, ids after the large payload as
            # Codex serializes them; the process keeps serving other threads afterwards.
            big = 'x' * (17 * 1024 * 1024)
            emit({'method': 'item/completed', 'params': {
                'item': {'id': 'command-' + turn_id, 'type': 'commandExecution',
                'aggregatedOutput': big}, 'threadId': thread_id, 'turnId': turn_id}})
            emit({'method': 'turn/completed', 'params': {
                'threadId': thread_id, 'turn': {'id': turn_id, 'status': 'completed',
                'items': [{'text': big}]}}})
            continue
        if text in ('approve', 'approve-started'):
            if text == 'approve-started':
                emit({'method': 'item/started', 'params': {
                    'threadId': thread_id, 'item': {'id': 'command-' + turn_id,
                    'type': 'commandExecution', 'command': 'echo approved', 'status': 'running'}}})
            request_id = 900 + turns
            approvals[request_id] = ('approval', thread_id, turn_id)
            emit({'id': request_id, 'method': 'item/commandExecution/requestApproval',
                  'params': {'threadId': thread_id, 'itemId': 'command-' + turn_id,
                             'command': ['echo', 'approved'], 'reason': 'test'}})
        elif text == 'mcp-requests':
            # Browser tool approval, an unsupported request, then another server's approval.
            # The turn finishes once the other server's approval is answered.
            base = 800 + turns * 10
            approval_meta = {'codex_approval_kind': 'mcp_tool_call', 'persist': ['session']}
            def elicitation(server):
                return {'threadId': thread_id, 'turnId': turn_id, 'serverName': server,
                        'mode': 'form', 'message': 'Allow ' + server + ' to run tool?',
                        '_meta': approval_meta,
                        'requestedSchema': {'type': 'object', 'properties': {}}}
            approvals[base + 1] = ('log:browser', thread_id, turn_id)
            emit({'id': base + 1, 'method': 'mcpServer/elicitation/request',
                  'params': elicitation('ketplus_browser')})
            approvals[base + 2] = ('log:unknown', thread_id, turn_id)
            emit({'id': base + 2, 'method': 'item/permissions/requestApproval',
                  'params': {'threadId': thread_id, 'turnId': turn_id, 'itemId': 'p'}})
            approvals[base + 3] = ('log:url', thread_id, turn_id)
            emit({'id': base + 3, 'method': 'mcpServer/elicitation/request',
                  'params': {'threadId': thread_id, 'serverName': 'docs', 'mode': 'url',
                             'elicitationId': 'e', 'message': 'Sign in',
                             'url': 'https://example.com'}})
            approvals[base + 4] = ('log-finish:other', thread_id, turn_id)
            emit({'id': base + 4, 'method': 'mcpServer/elicitation/request',
                  'params': elicitation('docs')})
        elif text == 'unknown-input':
            # ADR 0015 §5: an unknown notification, a known one missing a required field (twice),
            # an item of an unknown type, then an unknown server request that ends the turn.
            emit({'method': 'thread/futureSignal', 'params': {'threadId': thread_id}})
            emit({'method': 'thread/futureSignal', 'params': {'threadId': thread_id}})
            for _ in range(2):
                emit({'method': 'item/completed', 'params': {
                    'threadId': thread_id, 'item': {'type': 'agentMessage', 'text': 'lost'}}})
            emit({'method': 'item/completed', 'params': {
                'threadId': thread_id, 'item': {'id': 'future-' + turn_id,
                'type': 'futureItem'}}})
            request_id = 600 + turns
            approvals[request_id] = ('log-finish:future', thread_id, turn_id)
            emit({'id': request_id, 'method': 'item/future/request',
                  'params': {'threadId': thread_id, 'turnId': turn_id}})
        elif text == 'mcp-untrusted':
            # An untagged field-less form, then a ketplus_browser tool approval. Without injected
            # launch arguments the latter is a user-configured server and needs a card.
            base = 700 + turns * 10
            approvals[base + 1] = ('log:untagged', thread_id, turn_id)
            emit({'id': base + 1, 'method': 'mcpServer/elicitation/request',
                  'params': {'threadId': thread_id, 'turnId': turn_id,
                             'serverName': 'ketplus_browser', 'mode': 'form',
                             'message': 'Continue?',
                             'requestedSchema': {'type': 'object', 'properties': {}}}})
            approvals[base + 2] = ('log-finish:browser', thread_id, turn_id)
            emit({'id': base + 2, 'method': 'mcpServer/elicitation/request',
                  'params': {'threadId': thread_id, 'turnId': turn_id,
                             'serverName': 'ketplus_browser', 'mode': 'form',
                             'message': 'Allow ketplus_browser to run tool?',
                             '_meta': {'codex_approval_kind': 'mcp_tool_call'},
                             'requestedSchema': {'type': 'object', 'properties': {}}}})
        elif text in ('browser-tool', 'docs-tool'):
            # One MCP tool call on KetPlus' browser server or on another server.
            server = 'ketplus_browser' if text == 'browser-tool' else 'docs'
            for phase, status in (('item/started', 'inProgress'), ('item/completed', 'completed')):
                emit({'method': phase, 'params': {'threadId': thread_id, 'item': {
                    'id': 'tool-' + turn_id, 'type': 'mcpToolCall', 'server': server,
                    'tool': 'browser_pages', 'arguments': {}, 'status': status}}})
            finish(thread_id, turn_id)
        elif text == 'ask' and default_mode_request_user_input:
            request_id = 900 + turns
            approvals[request_id] = ('input', thread_id, turn_id)
            emit({'id': request_id, 'method': 'item/tool/requestUserInput',
                  'params': {'threadId': thread_id, 'turnId': turn_id,
                             'itemId': 'question-' + turn_id, 'isBlocking': False,
                             'questions': [{
                                 'id': 'scope', 'header': 'Scope',
                                 'question': 'Which scope should the agent use?',
                                 'options': [{
                                     'label': 'Current workspace',
                                     'description': 'Limit the change to this workspace.'
                                 }]
                             }]}})
        else:
            threading.Timer(0.12, finish, args=(thread_id, turn_id)).start()
    elif method == 'mcpServer/tool/call':
        log('toolcall:' + json.dumps(message['params'], sort_keys=True))
        emit({'id': message_id, 'result': {'content': []}})
    elif method == 'turn/steer' or method == 'turn/interrupt':
        if method == 'turn/interrupt':
            log('interrupt:' + str(message['params'].get('turnId')))
        emit({'id': message_id, 'result': {}})
    elif method is None and message_id in approvals:
        kind, thread_id, turn_id = approvals.pop(message_id)
        result = message.get('result', {})
        if kind.startswith('log'):
            log('response:' + kind.split(':', 1)[1] + ':' + json.dumps(result, sort_keys=True))
            if kind.startswith('log-finish'):
                finish(thread_id, turn_id)
            continue
        if kind == 'input':
            answers = result.get('answers', {})
            log('input:' + json.dumps(answers, sort_keys=True))
        else:
            log('approval:' + result.get('decision', ''))
        finish(thread_id, turn_id)
)PY")
                               .arg(escapedLog);
    const QByteArray encoded = source.toUtf8();
    if (script.write(encoded) != encoded.size()) {
        return false;
    }
    script.close();
    return script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
}
