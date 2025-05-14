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

				// [MEMLEAK] Was: while ( (relation = MET_lookup_relation_id(tdbb, rel_id++, true)) )
				// That means, we return DELETED relations, instead rewriting them. That's why, if we reach
				// 32000+ operations, we got error.
				// Changing return_delete stmt to false fix this, but not full.
				while ( (relation = MET_lookup_relation_id(tdbb, rel_id, false)) )
				{
					// [MEMLEAK] Changing way of rel_id increment
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
				// [MEMLEAK] Was: Y.RDB$RELATION_ID = --rel_id;
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


## MSC_alloc & MSC_free version (Old)
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

