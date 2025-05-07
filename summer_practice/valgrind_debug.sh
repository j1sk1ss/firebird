#!bin/bash
valgrind --leak-check=full --track-origins=yes --log-file=firebird_valgrind.log /usr/local/firebird/bin -d
# [PRACTICE_MEMLEAK <date>]
# /usr/local/firebird/bin
