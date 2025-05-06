
import os
import sys
import fdb
import psutil
import argparse

PROC_NAME = 'firebird'
TEST_DB = '/tmp/test_mem.fdb'
ARCH = 'CS'

parser = argparse.ArgumentParser(description='Check memory consumption')
parser.add_argument("-a", "--arch", default=ARCH, choices=['CS', 'cs', 'SS', 'ss'])

args = parser.parse_args()
test_arch = args.arch

for proc in psutil.process_iter():
    if proc.name() == PROC_NAME:
        main_proc = psutil.Process(proc.pid)
        break

if not main_proc:
    sys.exit("Firebird process not found")

if os.path.isfile(TEST_DB):
    os.remove(TEST_DB)

conn = fdb.create_database(dsn='localhost:%s'%TEST_DB, user='sysdba', password='masterkey')
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

prev_used = server.memory_info().rss/1024                   # convert to kB

for i in range(1000):
    for j in range(1000):
        tmp_table = "test"
        cur.execute("create table %s(id int)"%tmp_table)
        conn.commit()
        cur.execute("drop table %s"%tmp_table)
        conn.commit()
    print('After %d queries:'%((i+1)*1000))
    result = server.memory_info().rss/1024                  # convert to kB
    diff = (result - prev_used)
    prev_used = result
    print('Used:        %d kB'%result)
    print('Difference:  %d kB'%diff)
    print('-'*25)
