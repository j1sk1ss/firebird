В проекте имеется модуль msc.cpp, включающий в себя MSC_alloc, который в свою очередь вызывается в функции create_table (src/gpre/cmd.cpp)
```
...

if (field->fld_default_value)
{
    put_blr(request, isc_dyn_fld_default_value, field->fld_default_value, CME_expr);
    TEXT* default_source = (TEXT*) MSC_alloc(field->fld_default_source->txt_length + 1);    
	CPR_get_text(default_source, field->fld_default_source);
    put_cstring(request, isc_dyn_fld_default_source, default_source); 
}

...

```

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

