# Minishell

A custom Linux shell implemented in C, with process management and dynamic memory allocation.

To use it, clone this repository into a Linux system, execute the compile.sh script and execute the executable file: 

```bash
git clone https://github.com/MarcosHM851/Minishell
cd Minishell
./compile.sh
./minishell
```

Use `< [file]` for stdin redirection from file, `> [file]` for stdout redirection to file and `>& [file]` for stderr redirection to file.
Use `|` to use various commands connected with pipes.
Use `exit` command to finish the program.
