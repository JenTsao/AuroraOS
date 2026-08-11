import os
import re

# Mapping of old field names to new contextual field names
# Note: we need to be careful with word boundaries and `->` or `.`
field_mapping = {
    # TaskContext
    'stack_ptr': 'task.stack_ptr',
    'privilege': 'task.privilege',
    'entry_point': 'task.entry_point',
    
    # SchedulerContext
    'state': 'scheduler.state',
    'id': 'scheduler.id',
    'sleep_ticks': 'scheduler.sleep_ticks',
    'base_priority': 'scheduler.base_priority',
    'current_priority': 'scheduler.current_priority',
    'next_ready': 'scheduler.next_ready',
    'prev_ready': 'scheduler.prev_ready',
    
    # MemoryContext
    'stack_base': 'memory.stack_base',
    'size_pow2': 'memory.size_pow2',
    'mpu_sandbox': 'memory.mpu_sandbox',
    'pgdir_base': 'memory.pgdir_base',
    'vasp_ptr': 'memory.vasp_ptr',
    
    # IpcContext
    'ipc_state': 'ipc.state',
    'ipc_blocked_next': 'ipc.blocked_next',
    'ipc_msg_buf': 'ipc.msg_buf',
    'ipc_reply_buf': 'ipc.reply_buf',
    'ipc_msg_len': 'ipc.msg_len',
    'ipc_max_len': 'ipc.max_len',
    'ipc_sender_id': 'ipc.sender_id',
    'ipc_receiver_id': 'ipc.receiver_id',
    'ipc_msg_type': 'ipc.msg_type',
    'notify_value': 'ipc.notify_value',
    'notify_pending': 'ipc.notify_pending',
    
    # SecurityContext
    'cspace': 'security.cspace',
    'signal_mask': 'security.signal_mask',
    'pending_signals': 'security.pending_signals',
    'sig_actions': 'security.sig_actions',
    
    # Misc/Posix (put in TaskContext or SchedulerContext?)
    'errno_val': 'task.errno_val',
    'stack_canary_ptr': 'task.stack_canary_ptr',
    'held_mutexes': 'scheduler.held_mutexes',
    'waiting_on_mutex': 'scheduler.waiting_on_mutex',
}

def refactor_file(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    orig = content
    # We replace `obj->field` and `obj.field`
    for old, new in field_mapping.items():
        # Using negative lookbehind/lookahead to avoid matching substring of other variables
        # Match `->field` not followed by word chars
        content = re.sub(r'->' + old + r'(?!\w)', '->' + new, content)
        # Match `.field` not followed by word chars, and not preceded by `.` or `->` already containing new context
        # Actually it's safer to just replace `.field`
        content = re.sub(r'\.' + old + r'(?!\w)', '.' + new, content)
    
    if orig != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Refactored {filepath}")

def main():
    search_dirs = ['kernel', 'tests', 'boot', 'apps', 'net', 'ui', 'vfs', 'metrics', 'experimental', 'syscall']
    for d in search_dirs:
        for root, _, files in os.walk(d):
            for file in files:
                if file == 'task.hpp' or file == 'mpu.hpp':
                    continue
                if file.endswith('.cpp') or file.endswith('.hpp') or file.endswith('.c') or file.endswith('.h'):
                    refactor_file(os.path.join(root, file))

if __name__ == '__main__':
    main()
