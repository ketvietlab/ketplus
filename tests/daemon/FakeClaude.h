#pragma once

#include <QFile>
#include <QString>

inline bool writeFakeClaude(const QString& path, const QString& logPath) {
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly)) {
        return false;
    }
    QString escapedLog = logPath;
    escapedLog.replace('\\', "\\\\");
    escapedLog.replace('\'', "\\'");
    const QString source = QString::fromUtf8(R"PY(#!/usr/bin/env python3
import hashlib, json, sys, threading

log_path = '%1'
turns = 0
lock = threading.Lock()

def emit(value):
    with lock:
        print(json.dumps(value), flush=True)

def log(value):
    with open(log_path, 'a', encoding='utf-8') as output:
        output.write(value + '\n')

log('args:' + json.dumps(sys.argv[1:]))

def finish(session_id, turn_id, thread_id, text='finished'):
    item_id = 'claude-answer-' + turn_id
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'turn.started',
        'payload': {'turn_id': turn_id, 'thread_id': thread_id}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'item.started',
        'payload': {'item_id': item_id, 'item_type': 'agentMessage',
                    'item': {'id': item_id, 'type': 'agentMessage', 'text': '',
                             'status': 'inProgress'}}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'assistant.delta',
        'payload': {'item_id': item_id, 'delta': text}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'item.completed',
        'payload': {'item_id': item_id, 'item_type': 'agentMessage', 'text': text,
                    'item': {'id': item_id, 'type': 'agentMessage', 'text': text,
                             'status': 'completed'}}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'usage.session',
        'payload': {'mode': 'api_key', 'budgetUsd': 5, 'estimatedCostUsd': 12.5,
                    'inputTokens': 200, 'outputTokens': -1, 'raw_private': 'must-not-journal'}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'usage.context',
        'payload': {'tokenUsage': {'last': {'totalTokens': 85000},
                                   'modelContextWindow': 200000}}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'usage.limits',
        'payload': {'limits': [
            {'id': 'five_hour', 'label': 'Session', 'usedPercent': 41.5, 'resetsAt': 1790000000},
            {'id': 'over', 'label': 'Over', 'usedPercent': 140, 'resetsAt': 1790000000},
            {'id': '', 'label': 'Nameless', 'usedPercent': 5, 'resetsAt': 1790000000},
            {'id': 'unknown', 'label': 'Unknown', 'usedPercent': None, 'resetsAt': 1790000000}]}}})
    emit({'method': 'event', 'params': {
        'sessionId': session_id, 'type': 'turn.completed',
        'payload': {'status': 'completed', 'thread_id': thread_id}}})

waiting = {}
hanging = {}

for line in sys.stdin:
    message = json.loads(line)
    method = message.get('method')
    message_id = message.get('id')
    params = message.get('params') or {}
    if method is None and message_id in waiting:
        log('permission:' + json.dumps(message.get('result') or message.get('error')))
        finish(*waiting.pop(message_id))
        continue
    if method == 'initialize':
        emit({'id': message_id, 'result': {'sdk': False}})
    elif method == 'model/list':
        emit({'id': message_id, 'result': {'data': [
            {'model': 'sonnet', 'displayName': 'Claude Sonnet', 'hidden': False,
             'provider': 'claude', 'defaultReasoningEffort': 'medium',
             'supportedReasoningEfforts': [{'reasoningEffort': 'low'},
                                           {'reasoningEffort': 'medium'},
                                           {'reasoningEffort': 'high'}]},
            {'model': 'opus', 'displayName': 'Claude Opus', 'hidden': False,
             'provider': 'claude', 'defaultReasoningEffort': 'high',
             'supportedReasoningEfforts': [{'reasoningEffort': 'low'},
                                           {'reasoningEffort': 'medium'},
                                           {'reasoningEffort': 'high'}]},
            {'model': 'claude-opus-4-6', 'displayName': 'Claude Opus 4.6', 'hidden': False,
             'provider': 'claude', 'defaultReasoningEffort': 'high',
             'supportedReasoningEfforts': [{'reasoningEffort': 'low'},
                                           {'reasoningEffort': 'medium'},
                                           {'reasoningEffort': 'high'},
                                           {'reasoningEffort': 'xhigh'},
                                           {'reasoningEffort': 'max'}]}]}})
    elif method == 'turn/start':
        log('setup:' + json.dumps(params))
        log('browser:' + json.dumps(params.get('mcpServers')))
        log('policy:' + json.dumps({'instructions': params.get('browserInstructions'), 'prompt': params.get('prompt'), 'images': params.get('images')}))
        turns += 1
        session_id = params.get('sessionId')
        turn_id = 'claude-turn-' + str(turns)
        thread_id = params.get('resume') or ('claude-thread-' + str(turns))
        log('turn:' + session_id + ':' + params.get('prompt', ''))
        emit({'id': message_id, 'result': {
            'turn': {'id': turn_id}, 'thread': {'id': thread_id}}})
        params['prompt'] = params.get('prompt', '').split('\n\n<ketplus_response_preferences>')[0]
        if params.get('prompt') == 'needs-approval':
            waiting['perm-' + str(turns)] = (session_id, turn_id, thread_id)
            emit({'id': 'perm-' + str(turns), 'method': 'permission.request', 'params': {
                'sessionId': session_id, 'tool': 'Bash', 'input': {'command': 'ls -la'}}})
        elif params.get('prompt') == 'hang' or 'please hang' in params.get('prompt', ''):
            hanging[session_id] = turn_id
            emit({'method': 'event', 'params': {
                'sessionId': session_id, 'type': 'turn.started',
                'payload': {'turn_id': turn_id, 'thread_id': ''}}})
        elif params.get('prompt') == 'background':
            emit({'method': 'event', 'params': {
                'sessionId': session_id, 'type': 'tasks.changed',
                'payload': {'tasks': [
                    {'task_id': 't1', 'kind': 'shell', 'title': 'Sleep', 'status': 'running',
                     'command': 'sleep 100', 'started_at': 1790000000000, 'has_output': True},
                    {'task_id': 'bad', 'kind': 'shell', 'title': 'Bad', 'status': 'exploded'}]}}})
            finish(session_id, turn_id, thread_id)
        elif params.get('prompt') == 'slow-start':
            def late(session_id=session_id, turn_id=turn_id, thread_id=thread_id):
                finish(session_id, turn_id, thread_id)
            threading.Timer(0.2, late).start()
        elif 'no-findings' in params.get('prompt', ''):
            finish(session_id, turn_id, thread_id, 'Looks fine.\nVerdict: no findings')
        else:
            finish(session_id, turn_id, thread_id)
    elif method == 'thread/fork':
        log('fork:' + json.dumps(params, sort_keys=True))
        emit({'id': message_id, 'result': {'thread': {'id': 'fork-of-' + params['threadId']}}})
    elif method == 'turn/steer':
        log('steer:' + str(params.get('sessionId')) + ':' + str(params.get('prompt')))
        for image in params.get('images', []):
            with open(image, 'rb') as data:
                log('steer-image:' + hashlib.sha256(data.read()).hexdigest())
        params['prompt'] = params.get('prompt', '').split('\n\n<ketplus_response_preferences>')[0]
        if params.get('prompt') == 'unanswered steer':
            pass
        elif params.get('prompt') == 'refused steer':
            emit({'id': message_id, 'error': {'message': 'The agent refused the message.'}})
        elif params.get('sessionId') in hanging:
            emit({'id': message_id, 'result': {}})
        else:
            emit({'id': message_id, 'error': {'message': 'The Claude turn has already finished.'}})
    elif method == 'task/output':
        log('task-output:' + params.get('taskId', ''))
        emit({'id': message_id, 'result': {'output': 'tick\n' * 3000, 'total_bytes': 15000,
                                           'truncated': False}})
    elif method == 'task/stop':
        log('task-stop:' + params.get('taskId', ''))
        emit({'id': message_id, 'result': {}})
        emit({'method': 'event', 'params': {
            'sessionId': params.get('sessionId'), 'type': 'tasks.changed',
            'payload': {'tasks': [{'task_id': params.get('taskId'), 'kind': 'shell',
                                   'title': 'Sleep', 'status': 'stopped'}]}}})
    elif method == 'turn/interrupt':
        log('interrupt:' + str(params.get('sessionId')))
        emit({'id': message_id, 'result': {}})
        if params.get('sessionId') in hanging:
            hanging.pop(params.get('sessionId'))
            emit({'method': 'event', 'params': {
                'sessionId': params.get('sessionId'), 'type': 'turn.completed',
                'payload': {'status': 'interrupted', 'thread_id': ''}}})
    elif message_id is not None:
        emit({'id': message_id, 'error': {'message': 'Unknown method'}})
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
