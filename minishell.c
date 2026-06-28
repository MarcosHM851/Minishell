#include "parser.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  pid_t *pid; // Array de PID de los procesos
  int id;     // ID del job
  char *name;
  int state; // 0 -> bg, 1 -> stopped
  int nprocess;
} tjob;

// VARIABLES GLOBALES
tjob *job_list;  // Lista de jobs
tline *line;     // Línea de comandos actual, contiene información sobre los mandatos de la linea
pid_t *pid;      // Array de pid que almacena los procesos en primer plano
int job_counter; // Contador que indica el número total de trabajos almacena en la lista de trabajos

/*Variable que me indica si estoy o no en el prompt (1=
Estamos esperando a que escriba el usuario 0= Se esta ejecutando una serie de comandos de un job)*/
volatile int prompt;
// MANEJADORES DE SEÑALES
void sigchild_handler() {
  signal(SIGCHLD, sigchild_handler);
}

void sigint_handler() {

  printf("\n");

  if (prompt == 1) {
    printf("msh> ");
    fflush(stdout);
  } else if (line != NULL && pid != NULL && line->ncommands > 0) { // Se comprueba que haya procesos en ejecución
    kill(-pid[0], SIGINT);
  }
}

void sigtstp_handler() {
  printf("\n");

  if (prompt == 1) {
    printf("msh> ");
    fflush(stdout);
  } else if (line != NULL && pid != NULL && line->ncommands > 0) {
    kill(-pid[0], SIGTSTP); // PID NEGATIVO: el kill se envía a un grupo de procesos.
  }
}

// FUNCIONES

void restore_and_close_fds(int stdin_copy, int stdout_copy, int stderr_copy) {
  dup2(stdin_copy, 0);
  dup2(stdout_copy, 1);
  dup2(stderr_copy, 2);

  close(stdin_copy);
  close(stdout_copy);
  close(stderr_copy);
}

char *man_cd(tcommand *command) {
  char *cwd; // current working directory
  char *home;

  if (command->argc < 2) { // Si hemos recibido el mandato cd sin argumentos
    home = getenv("HOME"); // Obtenemos el escritorio home

    if (home == NULL) {
      fprintf(stderr, "cd: El directorio HOME no se encuentra\n");
      return NULL;
    }

    if (chdir(home) != 0) { // Devuelve 0 si no ha ocurrido ningún error o -1 si falló
      fprintf(stderr, "Error al cambiar de directorio: %s\n", strerror(errno));
      return NULL;
    }
  } else {
    // Si hemos recibido el mandato cd con argumentos
    if (chdir(command->argv[1]) != 0) {
      fprintf(stderr, "Error al cambiar de directorio: %s : %s\n",
              command->argv[1], strerror(errno));
      return NULL;
    }
  }
  cwd = getcwd(NULL, 0);
  return cwd;
}

void save_job(int job_c, tline *line, int new_state) {
  int total_len = 0;
  int c, a, p; // Indices para bucles for
  pid_t *temp;

  // Añadimos el job a la lista de jobs
  job_list[job_c - 1].id = job_c;
  job_list[job_c - 1].state = new_state;
  job_list[job_c - 1].nprocess = line->ncommands;

  // Reservamos memoria para almacenar el pid de los procesos, usando el número de procesos que tenga
  temp = realloc(job_list[job_c - 1].pid, line->ncommands * sizeof(pid_t));

  if (temp) {
    job_list[job_c - 1].pid = temp;
  } else { // En caso de que el realloc no haya funcionado
    fprintf(stderr, "Error: Fallo de memoria al redimensionar lista de PIDs (save_job)\n");
    exit(1);
  }

  // Guardamos el pid de cada proceso del job
  for (p = 0; p < job_list[job_c - 1].nprocess; p++) {
    job_list[job_c - 1].pid[p] = pid[p];
  }

  // Guardamos cada argumento:
  // Calculamos el espacio necesario para almacenar el nombre del mandato.

  for (c = 0; c < job_list[job_c - 1].nprocess; c++) { // Por cada comando hacemos
    for (a = 0; a < line->commands[c].argc; a++) {     // Por cada argumento del comando hacemos
      total_len += strlen(line->commands[c].argv[a]);  // Calcular tamaño de los argumentos del comando c
      total_len += 1;                                  // espacio
    }
    if (c < job_list[job_c - 1].nprocess - 1)
      total_len += 3; // espacio para " | "
  }

  total_len += 1;                                             // terminador nulo
  job_list[job_c - 1].name = calloc(total_len, sizeof(char)); // Reserva memoria y rellena con 0

  if (!job_list[job_c - 1].name) {
    fprintf(stderr, "Error: Fallo al intentar reservar memoria para guardar el nombre del trabajo en la lista (save_job)\n");
    exit(1);
  }

  for (c = 0; c < job_list[job_c - 1].nprocess; c++) {
    for (a = 0; a < line->commands[c].argc; a++) {
      strcat(job_list[job_c - 1].name, line->commands[c].argv[a]);
      strcat(job_list[job_c - 1].name, " ");
    }
    if (c < job_list[job_c - 1].nprocess - 1) {
      strcat(job_list[job_c - 1].name, "| ");
    }
  }
}

void clean_jobs_list() {
  int i, j, k, status, all_finished, any_stopped;
  pid_t result;
  tjob *temp;
  j = 0;

  for (i = 0; i < job_counter; i++) {
    all_finished = 1;
    any_stopped = 0;

    // Comprobamos todos los procesos de forma no bloqueante:
    for (k = 0; k < job_list[i].nprocess; k++) {
      if (job_list[i].pid[k] > 0) { // Si el proceso no se ha marcado como finalizado aun (pid[k] == 0) NO esperamos por el.
        result = waitpid(job_list[i].pid[k], &status, WNOHANG | WUNTRACED);
        if (result == 0) { // El proceso sigue en ejecución
          all_finished = 0;
        } else if (result > 0) { // El proceso está parado
          if (WIFSTOPPED(status)) {
            any_stopped = 1;
            all_finished = 0;
          } else { // El proceso ha finalizado
            job_list[i].pid[k] = 0;
          }
        } else { // Marcamos como finalizado
          job_list[i].pid[k] = 0;
        }
      }
    }

    if (all_finished) {
      if (job_list[i].state == 0) {
        printf("[%d]+  Done                    %s\n", job_list[i].id, job_list[i].name);
      }
      if (job_list[i].name != NULL) {
        free(job_list[i].name);
      }
      if (job_list[i].pid != NULL) {
        free(job_list[i].pid);
      }
    } else {
      // Si hay al menos un proceso del job parado (stopped), ponemos el estado global del job como parado
      if (any_stopped) {
        job_list[i].state = 1;
      }

      // La variable j se queda aparcada en el hueco que dejó un muerto,
      //  esperando para sobrescribirlo con el siguiente job vivo que encuentre
      //  la variable i
      if (i != j) {
        job_list[j] = job_list[i];
      }

      // Actualizamos el número del job (se suma 1 a j porque j empieza en 0 y los jobs se cuentan desde el 1)
      job_list[j].id = j + 1;
      j++;
    }
  }

  job_counter = j; // en este punto j ha contado el número de trabajos que quedan activos
  if (job_counter == 0) {
    // Si no quedan jobs activos, se libera la memoria de la lista de jobs y se pone a NULL
    if (job_list != NULL)
      free(job_list);
    job_list = NULL;
    // Si quedan jobs activos, encogemos el array para que en este quepan justo los trabajos en ejecución, sin espacios vacíos
  } else {
    temp = realloc(job_list, job_counter * sizeof(tjob));
    if (temp) {
      job_list = temp;
    }
  }
}

int main(void) {
  char *buf;                                    // Buffer utilizado para leer lineas por entrada estándar
  int i, j, k, x, z;                            // Indices de for
  int fd, stdin_copy, stdout_copy, stderr_copy; // Copias de los descriptores de fichero de stdin, stdout y
                                                // stderr para no perderlos en las redirecciones.
  int **pipe_fd;                                // Array de descriptores de fichero para pipes
  char *filename;
  char *cwd;
  int found;
  int pos;
  int job_id;
  pid_t pid_to_kill;
  mode_t currentUmask; // mode_t es un tipo entero para la funcion umask
  char *arg;
  int is_valid;
  long newUmask;
  int status;
  int job_saved;
  pid_t wpid;

  job_counter = 0;
  job_list = NULL;
  tjob *temp = NULL;

  buf = malloc(1024 * sizeof(char));

  // Comprobamos si el malloc se ha podido hacer, en caso contrario mostramos un error
  if (!buf) {
    fprintf(stderr, "Error: Fallo al intentar reservar memoria para el buffer que recibe la entrada éstandar (main)\n");
    exit(1);
  }

  signal(SIGCHLD, sigchild_handler); // SIGCHLD la recibe el padre cuando un hijo cambia de estado (de en ejecución a parado por ej)

  // Ignoramos Ctrl+Z por defecto (estamos en modo "prompt") (Esto evita guardar
  // el ultimo job ejecutado al pulsar ctrl+z, sino si pulsas varias veces se
  // guardaria muchas veces en la lista de jobs, cosa que no tiene sentido)
  signal(SIGTSTP, sigtstp_handler);

  printf("msh> ");

  // Se ignora la señal del crtl+c (En el manejador solo se hace print para
  // salto de linea)
  signal(SIGINT, sigint_handler);

  prompt = 1; // Estoy esperando a que el usuario escriba (en el prompt)
  while (fgets(buf, 1024, stdin)) {
    prompt = 0; // Estoy ejecutando la linea de mandatos escrita por el usuario
                // (Salimos del modo prompt)

    line = tokenize(buf);

    // Crear un array dinamico de tipo pid_t por cada comando de la linea
    if (line == NULL || line->ncommands == 0) { // Si el usuario no ha introducido ninguna línea, no creamos el array. Simplemente dibujamos el prompt de nuevo y volvemos al inicio del bucle.
      clean_jobs_list();
      printf("msh> ");
      prompt = 1;
      continue;
    } else { // Si el usuario ha introducido una línea reservamos memoria para el array de pid (Tantas posiciones como mandatos tenga el job)
      pid = malloc(line->ncommands * sizeof(pid_t));
      if (!pid) {
        fprintf(stderr, "Error: Fallo al intentar reservar memoria para el array de pid (main)\n");
        exit(1);
      }
    }

    if (line->background) { // Si ejecutamos el proceso en background

      // Incrementamos en 1 el número de trabajos vivos o en segundo plano
      job_counter++;

      // Reservamos memoria para guardar el nuevo proceso en la lista de
      // procesos
      temp = realloc(job_list, job_counter * sizeof(tjob)); // temp es un array temporal para evitar
                                                            // errores con el array completo.
      if (temp) {
        job_list = temp;
        // Inicializamos a null para que cuando se ejecute save_job, el realloc
        // actue como malloc la primera vez, sino pueden ocurrir errores de
        // segmentation fault
        job_list[job_counter - 1].pid = NULL;
        job_list[job_counter - 1].name = NULL;
      } else {
        fprintf(stderr, "Error: Fallo de memoria al redimensionar la lista de jobs (main)\n");
        exit(1);
      }
    }

    if (line->ncommands > 1) { // Si hay más de un comando en la linea:

      pipe_fd = malloc((line->ncommands - 1) * sizeof(int *)); // Array de punteros a enteros
      if (!pipe_fd) {
        fprintf(stderr, "Error: Fallo al intentar reservar memoria para el array de pipes (main)\n");
        exit(1);
      }
      for (i = 0; i < line->ncommands - 1; i++) {
        pipe_fd[i] = malloc(2 * sizeof(int)); // Reservamos en cada posición del array 2 posiciones,
                                              // que representarán los fd del respectivo pipe

        if (!pipe_fd[i]) {
          fprintf(stderr, "Error: Fallo al intentar reservar memoria para el array de pipes (main)\n");
          exit(1);
        }
      }
    }

    // Guardamos copias de los descriptores de fichero de entrada,salida,error
    // por si hay redirección para poder restaurarlos

    stdin_copy = dup(0);  // Guardamos una copia del descriptor de fichero de la entrada estandar
    stdout_copy = dup(1); // Guardamos una copia del descriptor de fichero de la salida éstandar
    stderr_copy = dup(2); // Guardamos una copia del descriptor de fichero de la salida de error

    // Redirección de entrada
    if (line->redirect_input != NULL) {
      filename = line->redirect_input;

      fd = open(line->redirect_input, O_RDONLY);

      // Si se produce un error al obtener/crear el archivo:
      if (fd == -1) {
        fprintf(stderr, "%s: Error. %s\n", filename, strerror(errno));

        restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

        // Liberamos el pid y pipe_fd ya que se van a volver a reservar en la
        // siguiente iteracion del bucle
        free(pid);

        if (line->ncommands > 1) {
          // Bucle para liberar pipe_fd[i] y pipe_fd
          for (i = 0; i < line->ncommands - 1; i++) {
            free(pipe_fd[i]);
          }

          free(pipe_fd);
        }

        clean_jobs_list();
        printf("msh> ");
        prompt = 1;
        continue;
      } else {
        dup2(fd, 0); // Redireccionar la entrada estandar
        close(fd);
      }
    }
    // Redirección de salida
    if (line->redirect_output != NULL) {
      filename = line->redirect_output;

      fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0666); // 0666 da permisos de lectura y escritura (rw-rw-rw-)

      if (fd == -1) {
        fprintf(stderr, "%s: Error: %s\n", filename, strerror(errno));

        restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

        // Liberamos el pid y pipe_fd ya que se van a volver a reservar en la
        // siguiente iteracion del bucle
        free(pid);

        if (line->ncommands > 1) {
          // Bucle para liberar pipe_fd[i] y pipe_fd
          for (i = 0; i < line->ncommands - 1; i++) {
            free(pipe_fd[i]);
          }
          free(pipe_fd);
        }
        clean_jobs_list();
        printf("msh> ");
        prompt = 1;
        continue;
      } else {
        dup2(fd, 1); // Redireccionar la salida estandar
        close(fd);
      }
    }
    // Redirección de error
    if (line->redirect_error != NULL) {
      filename = line->redirect_error;

      fd = open(line->redirect_error, O_WRONLY | O_CREAT | O_TRUNC, 0666);

      if (fd == -1) {
        fprintf(stderr, "%s: Error: %s\n", filename, strerror(errno));

        restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

        // Liberamos el pid y pipe_fd ya que se van a volver a reservar en la
        // siguiente iteracion del bucle
        free(pid);

        if (line->ncommands > 1) {
          // Bucle para liberar pipe_fd[i] y pipe_fd
          for (i = 0; i < line->ncommands - 1; i++) {
            free(pipe_fd[i]);
          }
          free(pipe_fd);
        }
        clean_jobs_list();
        printf("msh> ");
        prompt = 1;
        continue;
      }
      dup2(fd, 2); // Redireccionamos la salida de error
      close(fd);
    }

    // EJECUCIÓN DE LOS MANDATOS INTERNOS (cd, jobs, bg, exit, umask)

    if ((line->ncommands == 1) && (strcmp(line->commands[0].argv[0], "cd") == 0)) { // En este caso se tiene que ejecutar el mandato cd de forma interna (cd se ejecuta sin pipes por eso el número de mandatos debe ser 1)
      cwd = man_cd(&line->commands[0]);
      free(pid); // No vamos a llamar a ningun proceso hijo, liberamos el array
                 // de pid

      if (cwd) {
        printf("Directorio actual: %s\n", cwd);
      }

      // Antes de la siguiente iteración del fgets, restauramos los descriptores
      // de fichero originales
      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

      clean_jobs_list();
      printf("msh> "); // Imprimir el prompt, por el continue nos saltamos el
                       // printf del final
      free(cwd);
      prompt = 1; // Activar el modo prompt (Ahora va a esperar a que el usuario
                  // escriba, se queda esperando en while fgets...)
      continue;   // Salta a la siguiente iteración del while
    }

    if (strcmp(line->commands[0].argv[0], "jobs") == 0) {
      // Actualizamos la lista de jobs --> Se eliminan los trabajos que estaban
      // en background y ya han acabado antes de mostrarlos
      clean_jobs_list();

      // Mostramos la lista de trabajos

      for (i = 0; i < job_counter; i++) {
        if (job_list[i].state) {
          printf("[%d] Stopped                %s\n", job_list[i].id, job_list[i].name);
        } else {
          printf("[%d] Running             %s\n", job_list[i].id, job_list[i].name);
        }
      }
      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);
      clean_jobs_list();
      printf("msh> ");
      prompt = 1; // Activar el modo prompt (Ahora va a esperar a que el usuario escriba, se queda esperando en while fgets...)

      // Liberar pipes
      if (line->ncommands > 1) {
        // Bucle para liberar pipe_fd[i] y pipe_fd
        for (i = 0; i < line->ncommands - 1; i++) {
          free(pipe_fd[i]);
        }
        free(pipe_fd);
      }

      free(pid);
      continue;
    } else if (strcmp(line->commands[0].argv[0], "bg") == 0) {
      found = 0;
      j = 0;
      while (!found && j < job_counter) {
        if ((job_list[j].state) == 1) {
          found++;
        }
        j++;
      }

      if (!found) {
        fprintf(stderr, "Error: No hay procesos parados actualmente.\n");
        restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);
        clean_jobs_list();
        printf("msh> ");
        prompt = 1;

        if (line->ncommands > 1) {
          // Bucle para liberar pipe_fd[i] y pipe_fd
          for (i = 0; i < line->ncommands - 1; i++) {
            free(pipe_fd[i]);
          }
          free(pipe_fd);
        }

        free(pid);
        continue;
      }
      if (line->commands[0].argc == 1) { // Si el usuario solo ha escrito bg, bg
                                         // reanuda el ultimo job stopped
        pos = job_counter - 1;
        // Buscamos el último job stopped:
        while ((pos >= 0) && ((job_list[pos].state) == 0)) { // Mientras me queden trabajos en la lista y no estén stopped sigo buscando
          pos--;
        }
        // Si hemos salido del while y pos vale -1, entonces no hay jobs stopped
        if (pos == -1) {
          fprintf(stderr, "No hay jobs parados en este momento \n");

          restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

          clean_jobs_list();
          printf("msh> ");
          prompt = 1;

          if (line->ncommands > 1) {
            // Bucle para liberar pipe_fd[i] y pipe_fd
            for (i = 0; i < line->ncommands - 1; i++) {
              free(pipe_fd[i]);
            }
            free(pipe_fd);
          }

          free(pid);
          continue;
        } else { // Hemos encontrado un job stopped

          // Enviamos la señal a TODOS los procesos del pipe
          kill(-(job_list[pos].pid[0]), SIGCONT);

          job_list[pos].state = 0; // Job ejecutandose en background

          printf("[%d] Running             %s\n", job_list[pos].id, job_list[pos].name);
        }
      } else {
        job_id = atoi(line->commands[0].argv[1]);
        if (job_id - 1 < 0 || job_id - 1 >= job_counter || job_list[job_id - 1].state != 1) {
          fprintf(stderr, "Error: %s no es un valor válido como argumento de bg\n", line->commands[0].argv[1]);
          restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);
          clean_jobs_list();
          printf("msh> ");
          prompt = 1;

          if (line->ncommands > 1) {
            // Bucle para liberar pipe_fd[i] y pipe_fd
            for (i = 0; i < line->ncommands - 1; i++) {
              free(pipe_fd[i]);
            }
            free(pipe_fd);
          }

          free(pid);
          continue;
        } else {
          kill(-(job_list[job_id - 1].pid[0]), SIGCONT); // Enviamos SIGCONT a todo el grupo de procesos
          job_list[job_id - 1].state = 0;
          printf("[%d] Running             %s\n", job_list[job_id - 1].id, job_list[job_id - 1].name);
        }
      }

      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);
      clean_jobs_list();
      printf("msh> ");
      prompt = 1; // Activar el modo prompt (Ahora va a esperar a que el usuario
                  // escriba, se queda esperando en while fgets...)

      if (line->ncommands > 1) {
        // Bucle para liberar pipe_fd[i] y pipe_fd
        for (i = 0; i < line->ncommands - 1; i++) {
          free(pipe_fd[i]);
        }
        free(pipe_fd);
      }

      free(pid);
      continue;
    } else if (strcmp(line->commands[0].argv[0], "exit") == 0) { // Mandato exit

      // Matamos los procesos de cada job que este en la lista de jobs
      for (i = 0; i < job_counter; i++) { // Matamos de cada job, cada proceso del job
        for (j = 0; j < job_list[i].nprocess; j++) {
          pid_to_kill = job_list[i].pid[j];

          if (pid_to_kill > 0) {
            kill(pid_to_kill, SIGKILL);

            // Esperamos a que el proceso muera despues de mandarle la señal
            // SIGKILL, si no se hace esto el proceso se queda zombie unos
            // instantes
            waitpid(pid_to_kill, NULL, 0);
          }
        }

        // Liberamos los arrays que contienen los tjob antes de cerrar la
        // minishell
        free(job_list[i].name);
        free(job_list[i].pid);
      }

      // Liberamos memoria antes de cerrar la minishell
      free(job_list);
      free(buf);
      free(pid);

      // Liberamos los pipes solo si se ha ejecutado 2 o más comandos (Y por
      // tanto existe más de un pipe)
      if (line->ncommands > 1) {
        for (i = 0; i < line->ncommands - 1; i++) {
          free(pipe_fd[i]);
        }
        free(pipe_fd);
      }

      // Acabar de forma ordenada la minishell
      printf("Saliendo de la minishell... \n");
      exit(0);
    } else if ((line->ncommands == 1) && (strcmp(line->commands[0].argv[0], "umask") == 0)) { // MANDATO UMASK
      // SOLO EN MODO OCTAL

      if (line->commands[0].argc == 1) { // Si el usuario solo ha puesto umask sin argumentos

        // La función umask establece un valor de umask nuevo y devuelve el
        // anterior Para poder obtener el valor de umask actual debemos hacer
        // esto:
        currentUmask = umask(0);

        // Restauramos el valor de umask una vez obtenido
        umask(currentUmask);

        /*
        imprime en formato octal, 0 indica que ponga
        los espacios a la izquierda con ceros,
        y 4 indica que el número debe ocupar 4 caracteres de ancho como minimo
        */
        printf("%04o\n", currentUmask);

      } else { // Un argumento o más:
        arg = line->commands[0].argv[1];
        is_valid = 1;

        // Comprobamos que el número este en formato octal (numeros entre el 0 y el 7 incluidos)
        for (x = 0; arg[x] != '\0'; x++) {
          if (arg[x] < '0' || arg[x] > '7') {
            is_valid = 0;
            break;
          }
        }

        if (!is_valid) {
          fprintf(stderr, "Error: El argumento de umask debe ser un numero octal valido.\n");
        } else {
          newUmask = strtol(arg, NULL, 8);
          umask((mode_t)newUmask);
        }
      }

      clean_jobs_list();

      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

      printf("msh> ");
      prompt = 1; // Activar el modo prompt (Ahora va a esperar a que el usuario
                  // escriba, se queda esperando en while fgets...)
      free(pid);
      continue;
    }
    // Si se intenta ejecutar umask o cd dentro de una tubería mostramos un error
    else if ((strcmp(line->commands[0].argv[0], "umask") == 0) || (strcmp(line->commands[0].argv[0], "cd") == 0)) {
      fprintf(stderr, "No se puede ejecutar el mandato: %s usando pipes \n", line->commands[0].argv[0]);
      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);
      clean_jobs_list();
      printf("msh> ");
      prompt = 1;

      if (line->ncommands > 1) {
        // Bucle para liberar pipe_fd[i] y pipe_fd
        for (i = 0; i < line->ncommands - 1; i++) {
          free(pipe_fd[i]);
        }
        free(pipe_fd);
      }

      free(pid);
      continue;
    }
    // Si el mandato introducido no existe, mostramos un error (tokenize pone filename a NULL si no existe)
    else if (line->commands[0].filename == NULL) {
      restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

      fprintf(stderr, "%s: no se encuentra el mandato \n", line->commands[0].argv[0]);
      clean_jobs_list();
      printf("msh> ");
      prompt = 1;

      if (line->ncommands > 1) {
        // Bucle para liberar pipe_fd[i] y pipe_fd
        for (i = 0; i < line->ncommands - 1; i++) {
          free(pipe_fd[i]);
        }
        free(pipe_fd);
      }

      free(pid);
      continue;
    }

    // EJECUCIÓN DEL RESTO DE MANDATOS (En caso de no ser uno interno)

    // Creamos los pipes para la comunicación entre procesos del job
    for (j = 0; j < line->ncommands - 1; j++) {
      pipe(pipe_fd[j]);
    }

    for (i = 0; i < line->ncommands; i++) { // Recorremos cada comando de la linea

      pid[i] = fork(); // Para la ejecución de cada comando creamos un hijo

      if (pid[i] < 0) {
        fprintf(stderr, "Error al crear el proceso hijo");
      } else if (pid[i] == 0) { // Proceso hijo

        // Se crea un grupo de procesos para el primer comando
        if (i == 0) {
          setpgid(0, 0);
        }
        // Para el resto de procesos del job se les asignan el grupo de procesos creado para el primer comando
        else {
          setpgid(0, pid[0]);
        }

        // Asignamos el comportamiento por defecto al Ctrl+C y Ctrl+Z (desactivado anteriormente para el padre
        // para que la shell no se cierre/pause por error). Como el hijo hereda esa inmunidad al hacer fork,
        // usamos SIG_DFL para que este comando sí pueda ser cancelado o pausado por el usuario.

        signal(SIGTSTP, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        if (line->ncommands > 1) { // Si hay más de un comando en la línea

          /*Array de pipes:
          i: es el pipe siguiente al proceso actual
          i-1: es el pipe anterior al proceso actual*/

          if (i == 0) { // Si el proceso actual es el primer comando de la línea:
            close(pipe_fd[i][0]);
            dup2(pipe_fd[i][1], 1); // Se redirige la salida estándar al extremo de escritura del pipe

            // Es importante cerrar todos los descriptores de fichero que no se
            // vayan a usar. Por eso, una vez se hace dup2, se cierran todos los
            // pipes. De hecho, si esto no se hace en todos los mandatos se
            // producen interbloqueos.
            for (k = 0; k < line->ncommands - 1; k++) {
              close(pipe_fd[k][0]);
              close(pipe_fd[k][1]);
            }
          } else if (i == line->ncommands - 1) { // Si el proceso actual es el último de la línea
            close(pipe_fd[i - 1][1]);
            dup2(pipe_fd[i - 1][0], 0); // Se redirige la entrada estándar al extremo del array

            for (k = 0; k < line->ncommands - 1; k++) {
              close(pipe_fd[k][0]);
              close(pipe_fd[k][1]);
            }
          } else { // Si el proceso actual está en una posición media en la línea:
            close(pipe_fd[i - 1][1]);
            close(pipe_fd[i][0]);
            dup2(pipe_fd[i - 1][0], 0); // Se redirige la entrada estándar al extremo de lectura del pipe del anterior
            dup2(pipe_fd[i][1], 1);     // Se redirige la salida estándar al extremo de escritura del pipe siguiente.

            for (k = 0; k < line->ncommands - 1; k++) {
              close(pipe_fd[k][0]);
              close(pipe_fd[k][1]);
            }
          }
        }
        if (line->commands[i].filename == NULL) {
          fprintf(stderr, "%s: no se encuentra el mandato\n", line->commands[i].argv[0]);
          exit(1);
        } else {
          execvp(line->commands[i].filename, line->commands[i].argv);
        }

        // Si llegamos aqui ha ocurrido un error
        fprintf(stderr, "Error al ejecutar el mandato \n");
        // Antes de salir liberamos memoria

        // Liberamos el buffer de entrada (lo heredamos del padre)
        free(buf);

        // Liberamos el array de PIDs actual
        free(pid);

        // Liberamos la lista global de jobs
        if (job_list != NULL) {
          for (z = 0; z < job_counter; z++) { // Recorremos todos los trabajos de la lista, para liberar los atributos de cada job

            // Liberamos el string del nombre del job
            if (job_list[z].name != NULL) {
              free(job_list[z].name);
            }
            // Liberar el array de PIDs del job
            if (job_list[z].pid != NULL) {
              free(job_list[z].pid);
            }
          }
          // Liberamos la lista de jobs
          free(job_list);
        }

        // Liberamos los pipes si se crearon
        if (line->ncommands > 1) {
          for (k = 0; k < line->ncommands - 1; k++) {
            free(pipe_fd[k]);
          }
          free(pipe_fd);
        }

        exit(1);

      } // Cierre proceso hijo
      else { // Si el proceso es el padre, también debemos manejar su
             // pertenencia al grupo de procesos
        if (i == 0) {
          setpgid(pid[i], pid[i]);
        } else {
          setpgid(pid[i], pid[0]);
        }
      }
    } // Cierre del for que recorre cada comando de la línea (El padre vuelve al
      // inicio y crea el siguiente hijo)

    // PROCESO PADRE, ningún proceso hijo puede llegar hasta aquí dado el exec y
    // el exit en el bucle for. Guardamos el pid en caso de que se vayan a
    // ejecutar los mandatos en background, ya que el array pid se va a
    // sobreescribir en la siguiente iteración del while si queremos poder
    // ejecutar más procesos a la vez. El padre de nuevo ignora las señal del
    // crtl+c (En el manejador solo se hace print para salto de linea)
    signal(SIGINT, sigint_handler);

    if (line->background) {
      save_job(job_counter, line, 0);
      // Mostrar pid del proceso en Background (Se muestra el pid del ultimo
      // proceso de la linea de mandatos)
      printf("[%d] %d Running                %s\n", job_list[job_counter - 1].id, job_list[job_counter - 1].pid[line->ncommands - 1], job_list[job_counter - 1].name);
    }

    for (k = 0; k < line->ncommands - 1; k++) {
      close(pipe_fd[k][0]);
      close(pipe_fd[k][1]);
    }

    if (!line->background) { // Si el proceso no se ejecuta en segundo plano, esperamos de forma bloqueante al hijo
      job_saved = 0;         // Esta variable sirve para no guardar trabajos duplicados, ya que cuando se hace ctrl+z, la señal se envia
      // a todos los hijos y por tanto el bucle for de abajo haria tantos
      // waitpid como hijos haya

      for (k = 0; k < line->ncommands; k++) {
        // Con status obtenemos como terminó el hijo
        wpid = waitpid(pid[k], &status, WUNTRACED);

        // Solo miramos el estado si waitpid encontró al hijo
        // Si wpid es -1, significa que el sigchild_handler ya se encargó de él,
        // por tanto asumimos que ha terminado y lo ignoramos (Si no hacemos
        // esto es posible que a veces al hacer un crtl+z a un job y luego un
        // crtl+c a otro, se muestre como que el ultimo job ha quedado parado
        // (stopped) (debido al printf de mas abajo) como si se hubiese
        // ejecutado ctrl+z pero en realidad el job ha terminado)
        if (wpid > 0 && WIFSTOPPED(status)) {
          // Si es la primera vez que detectamos que este grupo se ha parado, lo
          // guardamos. (En un pipe, varios procesos se paran, pero solo
          // queremos 1 Job en la lista)
          if (job_saved == 0) {
            job_counter++;

            // Reasignamos memoria para guardar nuevo job
            temp = realloc(job_list, job_counter * sizeof(tjob));

            if (temp) {
              job_list = temp;
              // Inicializar a NULL para evitar fallos en save_job
              job_list[job_counter - 1].pid = NULL;
              job_list[job_counter - 1].name = NULL;

              // Guardamos el job con estado 1 (STOPPED)
              save_job(job_counter, line, 1);

              printf("[%d]+ Stopped \t\t %s\n", job_list[job_counter - 1].id, job_list[job_counter - 1].name);
            } else {
              fprintf(stderr, "Error: No se pudo reservar memoria para guardar el trabajo parado.\n");
              exit(1);
            }

            job_saved = 1; // Marcamos que ya hemos guardado el job stopped
          }
        }
      }
    }
    fflush(stdout);
    fflush(stderr);

    // Restauramos los descriptores de fichero de entrada,salida,error
    restore_and_close_fds(stdin_copy, stdout_copy, stderr_copy);

    // Liberamos memoria
    free(pid);

    // Liberamos los pipes si se han ejecutado más de 2 comandos
    if (line->ncommands > 1) {
      for (i = 0; i < line->ncommands - 1; i++) {
        free(pipe_fd[i]);
      }
      free(pipe_fd);
    }

    clean_jobs_list();
    printf("msh> ");

    // Hemos terminado de ejecutar, volvemos a estar esperando en el prompt a
    // que escriba el usuario
    prompt = 1; // Activar el modo prompt (Ahora va a esperar a que el usuario escriba, se queda esperando en while fgets...)
  }

  free(buf);

  if (job_list != NULL) {
    for (i = 0; i < job_counter; i++) {
      if (job_list[i].name != NULL) {
        free(job_list[i].name);
      }
      if (job_list[i].pid != NULL) {
        free(job_list[i].pid);
      }
    }
    free(job_list);
  }

  printf("\nSaliendo de la minishell...\n");

  return 0;
} // Cierre del int main