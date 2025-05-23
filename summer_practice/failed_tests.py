import os
import fdb

DB_NAME = '/tmp/failed_test.fdb'
if os.path.exists(DB_NAME):
	os.remove(DB_NAME)

conn_params = {
    'dsn': f'localhost:{DB_NAME}',
    'user': 'sysdba',
    'password': 'masterkey',
    'charset': 'UTF8',
}

test_statements = [
    "recreate table test(nm varchar(1) character set utf8 default 'qwerty' not null);",
    "recreate table test(nm varchar(1) character set utf8 default '€€€€€€' not null);",
    "create domain dm_ascii varchar(1) character set utf8 default 'qwertyu' not null;",
    "create domain dm_utf8 varchar(1) character set utf8 default '€€€€€€€' not null;",
    "recreate table test(nm dm_ascii);",
    "recreate table test(nm dm_utf8);",
    "create domain dm_utf8_nullable varchar(1) character set utf8 default '€€€€€€€€';",
    "recreate table test(nm dm_utf8_nullable not null);",
    "recreate table test(nm dm_utf8_nullable);",
    "alter domain dm_utf8_nullable set not null;",
    "recreate table test(id int);",
    "alter domain dm_utf8_nullable set not null;",
    "alter table test add nm2 varchar(1) character set utf8 default '€€' not null;",
    "alter table test add nm3 varchar(1) character set utf8 default '€€€', alter nm3 set not null;",
    "alter table test add nm4 varchar(3) character set utf8 default '€€€', alter nm4 type varchar(4), alter nm4 set default '€€€€€', alter nm4 set not null;",
    "alter table test add nm5 varchar(1) character set utf8, alter nm5 type dm_utf8_nullable;",
]

expected_error_parts = [
    "expected length 1, actual 6",
    "expected length 1, actual 6",
    None,  # create domain dm_ascii
    None,  # create domain dm_utf8
    "expected length 1, actual 7",
    "expected length 1, actual 7",
    None,  # create domain dm_utf8_nullable
    "expected length 1, actual 8",
    None,  # recreate table test(nm dm_utf8_nullable)
    "expected length 1, actual 8",
    None,  # recreate table test(id int)
    "expected length 1, actual 8",
    "expected length 1, actual 2",
    "expected length 1, actual 3",
    "expected length 4, actual 5",
    "expected length 1, actual 8",
]

def run_test():
    conn = fdb.create_database(dsn=f'localhost:{DB_NAME}', user='sysdba', password='masterkey')
    cur = conn.cursor()

    for i, sql in enumerate(test_statements):
        print(f"Running statement #{i+1}: {sql.strip()[:50]} ...")
        try:
            cur.execute(sql)
            conn.commit()
            if expected_error_parts[i]:
                print(f"Incorrect behaviour!")
            else:
                print(f"Correct behaviour")
        except fdb.DatabaseError as e:
            err_msg = str(e)
            if expected_error_parts[i]:
                if expected_error_parts[i] in err_msg:
                    print(f"Expected system error: {expected_error_parts[i]}")
                else:
                    print(f"Unexpected system error:")
                    print(f"    Predicted: {expected_error_parts[i]}")
                    print(f"    Received: {err_msg}")
            else:
                print(f"Unexpected error:")
                print(f"    {err_msg}")

    cur.close()
    conn.close()

if __name__ == "__main__":
    run_test()
