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

main_proc = None
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


import time


try:
    for i in range(test_count):
        total_create_time = 0.0
        total_commit_time = 0.0
        total_drop_time = 0.0

        total_create_mem = 0.0
        total_commit_mem = 0.0
        total_drop_mem = 0.0

        init_mem = prev_used
        for j in range(1000):
            start = time.perf_counter()
            cur.execute("create table test(id int)")
            total_create_time += time.perf_counter() - start
            total_create_mem += (server.memory_info().rss / 1024 - init_mem)
            init_mem = server.memory_info().rss / 1024

            start = time.perf_counter()
            conn.commit()
            total_commit_time += time.perf_counter() - start
            total_commit_mem += (server.memory_info().rss / 1024 - init_mem)
            init_mem = server.memory_info().rss / 1024

            start = time.perf_counter()
            cur.execute("drop table test")
            total_drop_time += time.perf_counter() - start
            total_drop_mem += (server.memory_info().rss / 1024 - init_mem)
            init_mem = server.memory_info().rss / 1024

            start = time.perf_counter()
            conn.commit()
            total_commit_time += time.perf_counter() - start
            total_commit_mem += (server.memory_info().rss / 1024 - init_mem)
            init_mem = server.memory_info().rss / 1024

        if server:
            print(f'After {((i + 1) * 1000)} queries:')
            result = server.memory_info().rss / 1024
            diff = (result - prev_used)
            prev_used = result

            print(f'Used:        {result} kB')
            print(f'Difference:  {diff} kB')

            print('-' * 25)
        else:
            print(f'Table CREATE & DROP cycle [{i}]')

        print(f'Average [CREATE] time: {total_create_time / 1000:.6f} s, Average mem increase: {total_create_mem / 1000:.6f} kB')
        print(f'Average [DROP] time:   {total_drop_time / 1000:.6f} s, Average mem increase: {total_drop_mem / 1000:.6f} kB')
        print(f'Avarage [COMMIT] time: {(total_commit_time / 2) / 1000:.6f} s, Average mem increase: {(total_commit_mem / 2) / 1000:.6f} kB')
        print('-' * 25)

except KeyboardInterrupt as _:
    pass
finally:
    _close_conn()

"""
Average diff (leak) is 3072KB => 3Kb per table CREATE & DROP (Found)
Average diff (cache leak) is 400KB => 409B per table CREATE & DROP + DROP operation slows with time.
That mean, this cache leak used during DROP operation, and when it goes larger, it slow down code.
From 0.000001s per operation to 0.000007s (x7 after 100000 CREATE & DROP operations).
"""
