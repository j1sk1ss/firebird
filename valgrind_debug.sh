#!bin/bash
valgrind --leak-check=full --track-origins=yes --log-file=firebird_valgrind.log firebird -d
# [PRACTICE_MEMLEAK <date>]
