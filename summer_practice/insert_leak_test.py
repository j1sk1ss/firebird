import os
import sys
import fdb
import psutil
import argparse

PROC_NAME = 'firebird'
TEST_DB = '/tmp/test_mem_data.fdb'
ARCH = 'CS'

parser = argparse.ArgumentParser(description='Check memory consumption for row inserts/deletes')
parser.add_argument("-a", "--arch", default=ARCH, choices=['CS', 'cs', 'SS', 'ss'])

args = parser.parse_args()
test_arch = args.arch

# Найти основной процесс Firebird
main_proc = None
for proc in psutil.process_iter():
    if proc.name() == PROC_NAME:
        main_proc = psutil.Process(proc.pid)
        break

if not main_proc:
    sys.exit("Firebird process not found")

# Удалить старую базу
if os.path.isfile(TEST_DB):
    os.remove(TEST_DB)

conn = fdb.create_database(dsn='localhost:%s' % TEST_DB, user='sysdba', password='masterkey')
cur = conn.cursor()
cur.execute('create table mem(str varchar(20))')
conn.commit()

if test_arch.lower() == 'cs':
    server = None
    for proc in psutil.process_iter():
        if proc.name() == PROC_NAME and proc.pid != main_proc.pid:
            server = psutil.Process(proc.pid)
            break
    if not server:
        sys.exit("Attachment process not found")
else:
    server = main_proc

prev_used = server.memory_info().rss / 1024  # KB

for i in range(1000):
    for j in range(1000):
        cur.execute("insert into mem(str) values(?)", ("test string",))
        conn.commit()
        cur.execute("delete from mem where str = ?", ("test string",))
        conn.commit()

    print(f'After {(i+1)*1000} operations:')
    result = server.memory_info().rss / 1024
    diff = result - prev_used
    prev_used = result
    print(f'Used:       {result:.0f} kB')
    print(f'Difference: {diff:.0f} kB')
    print('-' * 25)

cur.close()
conn.close()

"""
No memleaks found.
"""
