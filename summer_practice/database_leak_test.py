import os
import sys
import fdb
import psutil
import argparse


PROC_NAME = 'firebird'
DB__NAME = '/tmp/test_mem.fdb'

parser = argparse.ArgumentParser(description='Check memory consumption during CREATE/DROP database')
parser.add_argument("-u", "--user", default='sysdba')
parser.add_argument("-p", "--upass", default='masterkey')
parser.add_argument("-a", "--arch", default='CS', choices=['CS', 'cs', 'SS', 'ss'])
parser.add_argument("-c", "--count", type=int, default=100)

args = parser.parse_args()
test_arch: str = args.arch.upper()
test_count: int = args.count
test_user: str = args.user
test_user_pass: str = args.upass


main_proc = None
for proc in psutil.process_iter():
    if proc.name() == PROC_NAME:
        main_proc = psutil.Process(proc.pid)
        break

if not main_proc:
    print("[WARN] Firebird process not found. Maybe it is running under valgrind?")

if test_arch == 'CS':
    server = None
    for proc in psutil.process_iter():
        if proc.name() == PROC_NAME and proc.pid != main_proc.pid:
            server = psutil.Process(proc.pid)
            break
    if not server:
        sys.exit("Attachment process not found")
else:
    server = main_proc

if server:
    prev_used = server.memory_info().rss / 1024
else:
    prev_used = 0

print(f'Initial memory usage: {prev_used:.2f} kB')
print('=' * 30)

for i in range(test_count):
	for j in range(10):
		if os.path.exists(DB__NAME):
			os.remove(DB__NAME)

		conn = fdb.create_database(dsn=f'localhost:{DB__NAME}', user=test_user, password=test_user_pass)
		cur = conn.cursor()
		cur.execute("create table test(id int)")
		conn.commit()
		cur.close()
		conn.close()

		if os.path.exists(DB__NAME):
			os.remove(DB__NAME)

	if server:
		current = server.memory_info().rss / 1024
		diff = current - prev_used
		print(f'[{i+1}] Used: {current:.2f} kB | Diff: {diff:.2f} kB')
		prev_used = current

print('=' * 30)
print("Done.")

"""
No memleaks found.
"""
