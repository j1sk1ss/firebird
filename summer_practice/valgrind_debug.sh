#!bin/bash
sudo valgrind --leak-check=full --show-leak-kinds=all --trace-children=yes --track-origins=yes --log-file=firebird_valgrind.log ./firebird
# [PRACTICE_MEMLEAK <date>]
# /usr/local/firebird/bin

# gdb --args /usr/local/firebird/bin/fbguard -forever
# (gdb) set follow-fork-mode child
# (gdb) set detach-on-fork off
# (gdb) handle SIGSEGV stop noprint
# (gdb) run

# CREATE DATABASE '/home/j1sk1ss/Downloads/mydb.fdb' USER 'SYSDBA' PASSWORD 'masterkey' DEFAULT CHARACTER SET UTF8;
