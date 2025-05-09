# MSC_alloc & MSC_free version
------------------------------

Функции которые вызывают MSC_alloc и вызываются во время тестового кейса (иными словами создают мемлик):
CMP_t_start
act_set_transaction
CMP_compile_request

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

