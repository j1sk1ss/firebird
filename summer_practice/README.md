# Server does not release memory after DDL statements. #7318 (Opened)

## Cache leak version (Current)
------------------------------

Основная идея в том, что при создании и удалении таблиц, названия и метаданные кешируются, но не удаляются при коммите. 
Именно из-за этого, в режиме архитектуры супер-сервера происходит накопление кеша в рамках одного подключения. Если же мы
закрываем это подключение - кеш чистится. Раз происходит полная очистка - память никуда не утекает. Этот же вывод делает
`vagrind`. </br>

`create_relation` Функция, вызываемая для создания мета-информации об обьектах для быстрого доступа к ним. Основной
проблемой было то, что удалённые обьекты, не очищались должным образом. Решением стало их очистка далее. Для проверки гипотезы
был изменён исходный код так, что бы мы перезаписывали уже удалённые записи. </br>
Итогом стало изменение "Утечки" с 4МБ до 400КБ.

[dfw.epp](https://github.com/j1sk1ss/firebird/blob/master/src/jrd/dfw.epp#L3635-L3763)
```
		rel_id = 0;
		request.reset(tdbb, irq_c_relation, IRQ_REQUESTS);

		FOR(REQUEST_HANDLE request TRANSACTION_HANDLE transaction)
			X IN RDB$DATABASE CROSS Y IN RDB$RELATIONS WITH
				Y.RDB$RELATION_NAME EQ work->dfw_name.c_str()
		{
			blob_id = Y.RDB$VIEW_BLR;
			external_flag = Y.RDB$EXTERNAL_FILE[0];

			MODIFY X USING
				rel_id = X.RDB$RELATION_ID;

				if (rel_id < local_min_relation_id || rel_id > MAX_RELATION_ID)
					rel_id = X.RDB$RELATION_ID = local_min_relation_id;

				// Roman Simakov: We need to return deleted relations to skip them.
				// This maybe result of cleanup failure after phase 3.

				while ( (relation = MET_lookup_relation_id(tdbb, rel_id, true)) )
				{
/*
[PRACTICE] We care about REL_deleted records. If a record is used in only one or fewer transactions, we choose the current rel_id.
I also changed the mechanism for incrementing rel_id. Now we increment rel_id only if we can't use that record.
*/
					if (relation->rel_flags & REL_deleted && relation->rel_use_count <= 1) {
						break;
					}

					rel_id++;
					if (rel_id < local_min_relation_id || rel_id > MAX_RELATION_ID)
						rel_id = local_min_relation_id;

					if (rel_id == X.RDB$RELATION_ID)
					{
						ERR_post(Arg::Gds(isc_no_meta_update) <<
								 Arg::Gds(isc_table_name) << Arg::Str(work->dfw_name) <<
								 Arg::Gds(isc_imp_exc));
					}
				}

				X.RDB$RELATION_ID = (rel_id > MAX_RELATION_ID) ? local_min_relation_id : rel_id;

				MODIFY Y USING
					Y.RDB$RELATION_ID = rel_id;
					if (blob_id.isEmpty())
						Y.RDB$DBKEY_LENGTH = 8;
					else
					{
						// update the dbkey length to include each of the base relations
						Y.RDB$DBKEY_LENGTH = 0;

						handle.reset();

						FOR(REQUEST_HANDLE handle)
							Z IN RDB$VIEW_RELATIONS CROSS
							R IN RDB$RELATIONS OVER RDB$RELATION_NAME
							WITH Z.RDB$VIEW_NAME = work->dfw_name.c_str() AND
								 (Z.RDB$CONTEXT_TYPE = VCT_TABLE OR
								  Z.RDB$CONTEXT_TYPE = VCT_VIEW)
						{
							Y.RDB$DBKEY_LENGTH += R.RDB$DBKEY_LENGTH;
						}
						END_FOR
					}
				END_MODIFY
			END_MODIFY
		}
		END_FOR
```

Главный вопрос в том, можно ли просто так перезаписывать помеченные на удаление записи? Сами записи связанный с `tra_resources` 
и добавляются туда из `TRA_post_resources`. Очистка происходит в `TRA_release_transaction` (`MET_release_existence`). </br>
Если смотреть на `tra_resources` в момент "перезаписи", можно подметить то, что в них не содержатся данные об отношении.
Проверить это можно следующим образом:
```
for (rsc = transaction->tra_resources.begin(); rsc < transaction->tra_resources.end(); rsc++) {
    if (rsc->rsc_rel == relation) break;
}
```
Иными словами, теперь мы перезаписываем только если это отношение есть в нынешней транзакции и нигде более, 
то есть мы гарантируем что мы явно знаем где сейчас это отношение. Но по результатам тестирования, такой случай ни разу
не был встречен. </br>
 
`DFW_perform_post_commit_work` [dfw.epp](https://github.com/j1sk1ss/firebird/blob/master/src/jrd/dfw.epp#L1649-L1715) - точка 
где по результату тестов мы можем интегрировать очистку кеша. Так следующий блок был добавлен в эту функцию:
```
Jrd::Attachment* attachment = transaction->getAttachment();
vec<jrd_rel*>& rels = *attachment->att_relations;
for (FB_SIZE_T i = 0; i < rels.count(); i++) {
	jrd_rel* relation = rels[i];
	if (relation && (relation->rel_flags & REL_deleted) && relation->rel_use_count <= 0) {
		relation->cleanUp();
		delete rels[i];
		rels[i] = NULL;
	}
}
```


### core_6414_test failed (Solved)
------------------------------

Start at 23.05
```
tests.bugs.core_1245_test.test_1 						- Regression
tests.bugs.core_3141_test.test_1 						- Regression
tests.bugs.core_3362_basic_test.test_1 					- Regression
tests.bugs.core_6336_test.test_1 						- Regression
tests.bugs.core_6414_test.test_1 						- Regression
tests.bugs.gh_7062_test.test_1 							- Regression
tests.bugs.gh_8168_test.test_1 							- Regression
tests.functional.domain.create.test_26.test_1 			- Regression
tests.functional.domain.create.test_27.test_1 			- Regression
tests.functional.domain.create.test_28.test_1 			- Regression
tests.functional.domain.create.test_29.test_1 			- Regression
tests.functional.domain.create.test_31.test_1 			- Regression
tests.functional.replication.test_oltp_emul_ddl.test_1 	- Regression
```

После изменения исходного кода, перестали проходить вышеописанные тесты. Для проверки будет проведён откат
до версии из `main` и прогнан скрипт `failed_tests.py`. Так же будет проведён тест изменённой версии. </br>
Есть идея, что проблема в не полной очистке отношений. (Всё таки оно оставляет мусор, как я понимаю).  
Под подозрением именно [MET_relation](https://github.com/j1sk1ss/firebird/blob/master/src/jrd/met.epp#L3676-L3735) и 
[METD_get_relation](https://github.com/j1sk1ss/firebird/blob/master/src/dsql/metd.epp#L1206-L1405). </br>
Сама ошибка происходит из-за неинициализированного указателя в `ddl.cpp`:

```
assign_field_length(field, resolved_type->intlsym_bytes_per_char);
resolved_type == NULL // true
```
Проблема в перезаписи отношений. Точка очистки отношений есть, и она в `releaseRelations` функции. </br>
На тысячу операций удаление и создание было вызванно 8 очисток всех отношений. С учётом того, что удаления и создания
происходят в разных транзакциях, это может быть проблемой. Но с другой стороны, при этом очищается весь вектор отношений.

```
❯ sudo python3 summer_practice/table_leak_test.py -a ss -c 1
After 1000 queries:
Used:        50744.0 kB
Difference:  4224.0 kB
-------------------------
Average [CREATE] time: 0.000912 s, Average mem increase: 0.128000 kB
Average [DROP] time:   0.001138 s, Average mem increase: 2.688000 kB
Avarage [COMMIT] time: 0.000836 s, Average mem increase: 0.704000 kB
-------------------------
❯ cat /tmp/release_log.log
reletaions freed!
reletaions freed!
reletaions freed!
reletaions freed!
reletaions freed!
reletaions freed!
reletaions freed!
reletaions freed!
```


## MSC_alloc & MSC_free version (Old target)
------------------------------

Result 08.05: No, there is no memaleak. This is a cache leak. </br>

Функции которые вызывают MSC_alloc и вызываются во время тестового кейса (иными словами создают мемлик): </br>
`CMP_t_start` </br>
`act_set_transaction` </br>
`CMP_compile_request` </br>

Сама функция MSC_alloc вызывается во время работы тестового кейса из https://github.com/FirebirdSQL/firebird/issues/7318, и как видно из логов (я сократил файлы), вызов изменяет адресс базового блока (файл msc_short.log).

```
No space! --> Block allocation!
size=48 space->spc_remaining=4048 addr=-288145344
size=48 space->spc_remaining=4000 addr=-288145344
size=48 space->spc_remaining=3952 addr=-288145344
size=48 space->spc_remaining=2080 addr=-288145344
size=48 space->spc_remaining=688 addr=-288145344
size=48 space->spc_remaining=640 addr=-288145344
size=48 space->spc_remaining=592 addr=-288145344

...

size=48 space->spc_remaining=112 addr=-288145344
size=48 space->spc_remaining=64 addr=-288145344
size=48 space->spc_remaining=16 addr=-288145344
size=48 space->spc_remaining=4048 addr=-288141120
size=48 space->spc_remaining=4000 addr=-288141120
size=48 space->spc_remaining=3952 addr=-288141120
size=48 space->spc_remaining=3904 addr=-288141120

...

```

Даже учитывая то, что функция MSC_free не используется в приведённом семпле, сама по себе эта функция в принципе не реализована:

```

//____________________________________________________________
//
//		Free a block.
//

void MSC_free(void*)
{
}

```

Решением будет реализация очистки памяти в этом менеджере. Но сама структура не позволяет явно реиспользовать очищенную память. <\br>
По сути структура `msc` менеджера напоминает кучу из Линукса, которой можно упровлять через системный вызов `brk`, то есть <\br>
мы не можем явно разделять блоки памяти, а можем только двигать дальнюю границу. (В нашем случае добавлять новый блок \ удалять): <\br>

```

struct gpre_space
{
	gpre_space* spc_next;
	SLONG spc_remaining;
};

```

