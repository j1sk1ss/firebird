import os
import sys
import fdb
import psutil
import argparse

PROC_NAME = 'firebird'
TEST_DB = '/tmp/test_domain_leak.fdb'

parser = argparse.ArgumentParser(description='Check memory usage for CREATE DOMAIN / DROP DOMAIN')
parser.add_argument("-u", "--user", default='sysdba')
parser.add_argument("-p", "--upass", default='masterkey')
parser.add_argument("-a", "--arch", default='CS', choices=['CS', 'cs', 'SS', 'ss'])
parser.add_argument("-c", "--count", type=int, default=1000)
args = parser.parse_args()

test_arch = args.arch.upper()
test_count = args.count
test_user = args.user
test_user_pass = args.upass

# Найти основной процесс Firebird
main_proc = None
for proc in psutil.process_iter():
    if proc.name().lower().startswith(PROC_NAME):
        main_proc = psutil.Process(proc.pid)
        break

if not main_proc:
    print("[WARN] Firebird process not found. Maybe running under valgrind?")
    sys.exit(1)

# Удалить старую БД
if os.path.exists(TEST_DB):
    os.remove(TEST_DB)

# Создать новую БД и соединение
conn = fdb.create_database(dsn=f'localhost:{TEST_DB}', user=test_user, password=test_user_pass)
cur = conn.cursor()

# Получить серверный процесс
if test_arch == 'CS':
    server = None
    for proc in psutil.process_iter():
        if proc.name().lower().startswith(PROC_NAME) and proc.pid != main_proc.pid:
            server = psutil.Process(proc.pid)
            break
    if not server:
        print("Attachment process not found")
        conn.close()
        sys.exit(1)
else:
    server = main_proc

# Начальное использование памяти
prev_used = server.memory_info().rss / 1024
print(f'Initial memory usage: {prev_used:.2f} kB')
print("=" * 30)

# Основной цикл тестирования
try:
	for i in range(test_count):
		for i in range(10000):
			domain_name = f"dmn_{i}"
			cur.execute(f"create domain {domain_name} as varchar(100)")
			conn.commit()
			cur.execute(f"drop domain {domain_name}")
			conn.commit()

		current = server.memory_info().rss / 1024
		diff = current - prev_used
		print(f'After {i + 1} CREATE/DROP DOMAIN:')
		print(f'Used memory:  {current:.2f} kB')
		print(f'Difference:   {diff:.2f} kB')
		print('-' * 25)
		prev_used = current

except KeyboardInterrupt:
    print("\nInterrupted by user.")

finally:
    cur.close()
    conn.close()

"""
Average memory leak is 1536KB per 10000 operations -> 0.15RB per operation leak.
"""
