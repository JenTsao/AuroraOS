import os, re
p = 'kernel/core/ipc.hpp'
with open(p, 'rb') as f: data = f.read()
try: text = data.decode('gbk')
except: text = data.decode('utf-16le', errors='replace')
text = text.replace('#include "kernel_object.hpp"', '#include "kernel_object.hpp"\n#include "../task/wait_queue.hpp"')
text = text.replace(', send_head_(nullptr), send_tail_(nullptr), recv_head_(nullptr), recv_tail_(nullptr)', '')
text = re.sub(r'private:\s*void enqueue_sender.*?recv_tail_;', 'private:\n    WaitQueue send_queue_;\n    WaitQueue recv_queue_;', text, flags=re.DOTALL)
with open(p, 'w', encoding='utf-8') as f: f.write(text)
