"""
table_leak_test.py file testing memleak during CREATE TABLE and DROP TABLE.
-u / --user - Server user (default sysdba). Make sure that this user exists.
-p / --pass - Server user pass (default masterkey).
-a / --arch - Server type (default Classic server).
-c / --count - Test count (default 1000).
"""

import os
import sys
import fdb
import psutil
import argparse


PROC_NAME = 'firebird'
TEST_DB = '/tmp/test_mem.fdb'

parser = argparse.ArgumentParser(description='Check memory consumption')
parser.add_argument("-u", "--user", default='sysdba')
parser.add_argument("-p", "--upass", default='masterkey')
parser.add_argument("-a", "--arch", default='CS', choices=['CS', 'cs', 'SS', 'ss'])
parser.add_argument("-c", "--count", default=10000)

args = parser.parse_args()
test_arch: str = args.arch
test_count: int = args.count
test_user: str = args.user
test_user_pass: str = args.upass


"""
Find firebird process. If it launch under valgrind, will raise warning.
"""

for proc in psutil.process_iter():
    if proc.name() == PROC_NAME:
        main_proc = psutil.Process(proc.pid)
        break

if not main_proc:
    print("[WARN] Firebird process not found. Maybe it under valgrind?")


"""
Delete existed DB for new test cycle.
"""

if os.path.isfile(TEST_DB):
    os.remove(TEST_DB)


"""
Open db connection and creating test mem table
"""

conn: fdb.Connection = fdb.create_database(dsn=f'localhost:{TEST_DB}', user=test_user, password=test_user_pass)
cur = conn.cursor()
cur.execute('create table mem(str varchar(20))')
conn.commit()

if test_arch == 'CS':
    server = None
    for proc in psutil.process_iter():
        if (proc.name() == PROC_NAME) and (proc.pid != main_proc.pid):
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

"""
Main test loop.
"""

def _close_conn() -> None:
    cur.close()
    conn.close()

try:
    for i in range(test_count):
        for j in range(1000):
            cur.execute("create table test(id int)")
            conn.commit()
            cur.execute("drop table test")
            conn.commit()

        if server:
            print(f'After {((i + 1) * 1000)} queries:')
            result = server.memory_info().rss / 1024
            diff = (result - prev_used)
            prev_used = result

            print(f'Used:        {result} kB')
            print(f'Difference:  {diff} kB')
            print('-' * 25)
        else:
            print(f'CREATE & DROP cycle [{i}]')
except KeyboardInterrupt as _:
    _close_conn()

_close_conn()

"""
Average diff (leak) is 3072KB => 3Kb per table CREATE & DROP
"""
