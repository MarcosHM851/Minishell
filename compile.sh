gcc -Wall -Wextra -c minishell.c -o minishell.o
gcc minishell.o -no-pie -L. -lparser -o minishell
