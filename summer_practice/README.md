# Server does not release memory after DDL statements. #7318 (Opened)
## Cache leak version (Current)
------------------------------

Основная идея в том, что при создании и удалении таблиц, названия и метаданные кешируются, но не удаляются при коммите. 
Именно из-за этого, в режиме архитектуры супер-сервера происходит накопление кеша в рамках одного подключения. Если же мы
закрываем это подключение - кеш чистится. Раз происходит полная очистка - память никуда не утекает. Этот же вывод делает
`vagrind`. </br>
Необходимо найти ту часть кода, после которой кеш полностью очищается, и считать это за конец времени жизни раздутого
эллемента. После отследить момент добавления и наслоения данных в кеше. Решить проблему можно будет либо проверкой на уникальность,
либо на очистку кеша между транзакциями (что более логично, ведь кеш уже удалённой после дропа таблицы бесполезен). </br>
`METD_get_relation` является одной из подозреваемых функций, ведь тут происходит пометка кеша как дропнутого, без его очистки:
```
if (dbb->dbb_relations.get(name, temp) && !(temp->rel_flags & REL_dropped))
{
	if (MET_dsql_cache_use(tdbb, SYM_relation, name))
		temp->rel_flags |= REL_dropped;
	else
		return temp;
}
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

