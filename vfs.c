////////////////////////////////////////////////////////////////////////
//                                                                    //
//            Trabalho II: Sistema de Gestão de Ficheiros             //
//                                                                    //
// Compilação: gcc vfs.c -Wall -lreadline -o vfs                      //
// Utilização: ./vfs [-b[128|256|512|1024]] [-f[7|8|9|10]] FILESYSTEM //
//                                                                    //
////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <math.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAXARGS 100
#define CHECK_NUMBER 9999
#define TYPE_DIR 'D'
#define TYPE_FILE 'F'
#define MAX_NAME_LENGHT 20

#define FAT_ENTRIES(TYPE) ((TYPE) == 7 ? 128 : (TYPE) == 8 ? 256 : (TYPE) == 9 ? 512 : 1024)
#define FAT_SIZE(TYPE) (FAT_ENTRIES(TYPE) * sizeof(int))
#define BLOCK(N) (blocks + (N) * sb->block_size)
#define DIR_ENTRIES_PER_BLOCK (sb->block_size / sizeof(dir_entry))

typedef struct command {
  char *cmd;              // string apenas com o comando
  int argc;               // número de argumentos
  char *argv[MAXARGS+1];  // vector de argumentos do comando
} COMMAND;

typedef struct superblock_entry {
  int check_number;   // número que permite identificar o sistema como válido
  int block_size;     // tamanho de um bloco {128, 256 (default), 512 ou 1024 bytes}
  int fat_type;       // tipo de FAT {7, 8 (default), 9 ou 10}
  int root_block;     // número do 1º bloco a que corresponde o diretório raiz
  int free_block;     // número do 1º bloco da lista de blocos não utilizados
  int n_free_blocks;  // total de blocos não utilizados
} superblock;

typedef struct directory_entry {
  char type;                   // tipo da entrada (TYPE_DIR ou TYPE_FILE)
  char name[MAX_NAME_LENGHT];  // nome da entrada
  unsigned char day;           // dia em que foi criada (entre 1 e 31)
  unsigned char month;         // mes em que foi criada (entre 1 e 12)
  unsigned char year;          // ano em que foi criada (entre 0 e 255 - 0 representa o ano de 1900)
  int size;                    // tamanho em bytes (0 se TYPE_DIR)
  int first_block;             // primeiro bloco de dados
} dir_entry;

// variáveis globais
superblock *sb;   // superblock do sistema de ficheiros
int *fat;         // apontador para a FAT
char *blocks;     // apontador para a região dos dados
int current_dir;  // bloco do diretório corrente

// funções auxiliares
COMMAND parse(char *);
void parse_argv(int, char **);
void show_usage_and_exit(void);
void init_filesystem(int, int, char *);
void init_superblock(int, int);
void init_fat(void);
void init_dir_block(int, int);
void init_dir_entry(dir_entry *, char, char *, int, int);
void exec_com(COMMAND);

// funções de manipulação de diretórios
void vfs_ls(void);
void vfs_mkdir(char *);
void vfs_cd(char *);
void vfs_pwd(void);
void vfs_rmdir(char *);

// funções de manipulação de ficheiros
void vfs_get(char *, char *);
void vfs_put(char *, char *);
void vfs_cat(char *);
void vfs_cp(char *, char *);
void vfs_mv(char *, char *);
void vfs_rm(char *);


int main(int argc, char *argv[]) {
  char *linha;
  COMMAND com;

  parse_argv(argc, argv);
  while (1) {
    if ((linha = readline("vfs$ ")) == NULL)
      exit(0);
    if (strlen(linha) != 0) {
      add_history(linha);
      com = parse(linha);
      exec_com(com);
    }
    free(linha);
  }
  return 0;
}


COMMAND parse(char *linha) {
  int i = 0;
  COMMAND com;

  com.cmd = strtok(linha, " ");
  com.argv[0] = com.cmd;
  while ((com.argv[++i] = strtok(NULL, " ")) != NULL);
  com.argc = i;
  return com;
}


void parse_argv(int argc, char *argv[]) {
  int i, block_size, fat_type;

  // valores por omissão
  block_size = 256;
  fat_type = 8;
  if (argc < 2 || argc > 4) {
    printf("vfs: invalid number of arguments\n");
    show_usage_and_exit();
  }
  for (i = 1; i < argc - 1; i++) {
    if (argv[i][0] == '-') {
      if (argv[i][1] == 'b') {
  block_size = atoi(&argv[i][2]);
  if (block_size != 128 && block_size != 256 && block_size != 512 && block_size != 1024) {
    printf("vfs: invalid block size (%d)\n", block_size);
    show_usage_and_exit();
  }
      } else if (argv[i][1] == 'f') {
  fat_type = atoi(&argv[i][2]);
  if (fat_type != 7 && fat_type != 8 && fat_type != 9 && fat_type != 10) {
    printf("vfs: invalid fat type (%d)\n", fat_type);
    show_usage_and_exit();
  }
      } else {
  printf("vfs: invalid argument (%s)\n", argv[i]);
  show_usage_and_exit();
      }
    } else {
      printf("vfs: invalid argument (%s)\n", argv[i]);
      show_usage_and_exit();
    }
  }
  init_filesystem(block_size, fat_type, argv[argc-1]);
  return;
}


void show_usage_and_exit(void) {
  printf("Usage: vfs [-b[128|256|512|1024]] [-f[7|8|9|10]] FILESYSTEM\n");
  exit(1);
}


void init_filesystem(int block_size, int fat_type, char *filesystem_name) {
  int fsd, filesystem_size;

  if ((fsd = open(filesystem_name, O_RDWR)) == -1) {
    // o sistema de ficheiros não existe --> é necessário criá-lo e formatá-lo
    if ((fsd = open(filesystem_name, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU)) == -1) {
      printf("vfs: cannot create filesystem (%s)\n", filesystem_name);
      show_usage_and_exit();
    }

    // calcula o tamanho do sistema de ficheiros
    filesystem_size = block_size + FAT_SIZE(fat_type) + FAT_ENTRIES(fat_type) * block_size;
    printf("vfs: formatting virtual file-system (%d bytes) ... please wait\n", filesystem_size);

    // estende o sistema de ficheiros para o tamanho desejado
    lseek(fsd, filesystem_size - 1, SEEK_SET);
    write(fsd, "", 1);

    // faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
    if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
      close(fsd);
      printf("vfs: cannot map filesystem (mmap error)\n");
      exit(1);
    }
    fat = (int *) ((unsigned long int) sb + block_size);
    blocks = (char *) ((unsigned long int) fat + FAT_SIZE(fat_type));
    
    // inicia o superblock
    init_superblock(block_size, fat_type);
    
    // inicia a FAT
    init_fat();
    
    // inicia o bloco do diretório raiz '/'
    init_dir_block(sb->root_block, sb->root_block);
  } else {
    // calcula o tamanho do sistema de ficheiros
    struct stat buf;
    stat(filesystem_name, &buf);
    filesystem_size = buf.st_size;

    // faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
    if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
      close(fsd);
      printf("vfs: cannot map filesystem (mmap error)\n");
      exit(1);
    }
    fat = (int *) ((unsigned long int) sb + sb->block_size);
    blocks = (char *) ((unsigned long int) fat + FAT_SIZE(sb->fat_type));

    // testa se o sistema de ficheiros é válido 
    if (sb->check_number != CHECK_NUMBER || filesystem_size != sb->block_size + FAT_SIZE(sb->fat_type) + FAT_ENTRIES(sb->fat_type) * sb->block_size) {
      munmap(sb, filesystem_size);
      close(fsd);
      printf("vfs: invalid filesystem (%s)\n", filesystem_name);
      show_usage_and_exit();
    }
  }
  close(fsd);

  // inicia o diretório corrente
  current_dir = sb->root_block;
  return;
}


void init_superblock(int block_size, int fat_type) {
  sb->check_number = CHECK_NUMBER;
  sb->block_size = block_size;
  sb->fat_type = fat_type;
  sb->root_block = 0;
  sb->free_block = 1;
  sb->n_free_blocks = FAT_ENTRIES(fat_type) - 1;
  return;
}


void init_fat(void) {
  int i;

  fat[0] = -1;
  for (i = 1; i < sb->n_free_blocks; i++)
    fat[i] = i + 1;
  fat[sb->n_free_blocks] = -1;
  return;
}


void init_dir_block(int block, int parent_block) {
  dir_entry *dir = (dir_entry *) BLOCK(block);
  // o número de entradas no diretório (inicialmente 2) fica guardado no campo size da entrada "."
  init_dir_entry(&dir[0], TYPE_DIR, ".", 2, block);
  init_dir_entry(&dir[1], TYPE_DIR, "..", 0, parent_block);
  return;
}


void init_dir_entry(dir_entry *dir, char type, char *name, int size, int first_block) {
  time_t cur_time = time(NULL);
  struct tm *cur_tm = localtime(&cur_time);

  dir->type = type;
  strcpy(dir->name, name);
  dir->day = cur_tm->tm_mday;
  dir->month = cur_tm->tm_mon + 1;
  dir->year = cur_tm->tm_year;
  dir->size = size;
  dir->first_block = first_block;
  return;
}


void exec_com(COMMAND com) {
  // para cada comando invocar a função que o implementa
  if (!strcmp(com.cmd, "exit")) {
    exit(0);
  } else if (!strcmp(com.cmd, "ls")) {
    if (com.argc > 1)
      printf("ERROR(input: 'ls' - too many arguments)\n");
    else
      vfs_ls();
  } else if (!strcmp(com.cmd, "mkdir")) {
    if (com.argc < 2)
      printf("ERROR(input: 'mkdir' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'mkdir' - too many arguments)\n");
    else
      vfs_mkdir(com.argv[1]);
  } else if (!strcmp(com.cmd, "cd")) {
    if (com.argc < 2)
      printf("ERROR(input: 'cd' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'cd' - too many arguments)\n");
    else
      vfs_cd(com.argv[1]);
  } else if (!strcmp(com.cmd, "pwd")) {
    if (com.argc != 1)
      printf("ERROR(input: 'pwd' - too many arguments)\n");
    else
      vfs_pwd();
  } else if (!strcmp(com.cmd, "rmdir")) {
    if (com.argc < 2)
      printf("ERROR(input: 'rmdir' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'rmdir' - too many arguments)\n");
    else
      vfs_rmdir(com.argv[1]);
  } else if (!strcmp(com.cmd, "get")) {
    if (com.argc < 3)
      printf("ERROR(input: 'get' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'get' - too many arguments)\n");
    else
      vfs_get(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "put")) {
    if (com.argc < 3)
      printf("ERROR(input: 'put' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'put' - too many arguments)\n");
    else
      vfs_put(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "cat")) {
    if (com.argc < 2)
      printf("ERROR(input: 'cat' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'cat' - too many arguments)\n");
    else
      vfs_cat(com.argv[1]);
  } else if (!strcmp(com.cmd, "cp")) {
    if (com.argc < 3)
      printf("ERROR(input: 'cp' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'cp' - too many arguments)\n");
    else
      vfs_cp(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "mv")) {
    if (com.argc < 3)
      printf("ERROR(input: 'mv' - too few arguments)\n");
    else if (com.argc > 3)
      printf("ERROR(input: 'mv' - too many arguments)\n");
    else
      vfs_mv(com.argv[1], com.argv[2]);
  } else if (!strcmp(com.cmd, "rm")) {
    if (com.argc < 2)
      printf("ERROR(input: 'rm' - too few arguments)\n");
    else if (com.argc > 2)
      printf("ERROR(input: 'rm' - too many arguments)\n");
    else
      vfs_rm(com.argv[1]);
  } else
    printf("ERROR(input: command not found)\n");
  return;
}

////////////////////////////////
//EXTRAS
int get_free_block(){
  if(sb->n_free_blocks == 0) return -1;
  
  int block = sb->free_block;
  sb->free_block = fat[block];
  fat[block] = -1;
  
  sb->n_free_blocks --;
  return block;
}

void put_free_block(int block){
  fat[block] = sb->free_block;
  sb->free_block = block;
  sb->n_free_blocks ++;
  return;
}

////////////////////////////////


// ls - lista o conteúdo do diretório actual
void vfs_ls(void) {
  int my_dir = current_dir;
  dir_entry *dir = (dir_entry *) BLOCK(my_dir);
  int n_entry = dir[0].size;
  
  int k;
  for(int i=0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k == 0 && i!=0){
      my_dir = fat[my_dir];
      dir = (dir_entry *) BLOCK(my_dir);
    }
    printf("%-25s %02d-%02d-%04d",dir[k].name,dir[k].day,dir[k].month,dir[k].year+1900);
    if(dir[k].type == TYPE_DIR)
      printf(" DIR\n");
    else 
      printf(" %04d\n",dir[k].size);    
  }
  
  return;
}

// mkdir dir - cria um subdiretório com nome dir no diretório actual
void vfs_mkdir(char *nome_dir) {
  dir_entry *dir_o = (dir_entry *) BLOCK(current_dir), *dir = (dir_entry *) BLOCK(current_dir);
  int n_entry = dir_o[0].size;
  
  if(strlen(nome_dir) > 20){
    printf("ERROR(mkdir: cannot create directory '%s' - name too long)\n",nome_dir);
    return;
  }
  
  int i,k;
  int c_dir = current_dir;
  int n_blocks = 1;
  
  for(i = 0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k == 0 && i!=0){
      c_dir = fat[c_dir];
      dir = (dir_entry *) BLOCK(c_dir);
      n_blocks ++;
    }
    if(strcmp(nome_dir,dir[k].name) == 0) break;
  }
  
  if(i != n_entry){
    printf("ERROR(mkdir: cannot create directory '%s' - entry exists)\n",nome_dir);
    return;
  }
  
  int block = get_free_block();
  if(block == -1){
    printf("ERROR(mkdir: cannot create directory '%s' - disk is full)\n",nome_dir);
    return;
  }
  
  
  int my_dir = current_dir;
  while(fat[my_dir] != -1){
    my_dir = fat[my_dir];
    n_blocks++;
  }
  
  if(n_entry%DIR_ENTRIES_PER_BLOCK == 0){
    int block2 = get_free_block();
    if(block2 == -1){
      printf("ERROR(mkdir: cannot create directory '%s' - disk is full)\n",nome_dir);
      put_free_block(block);
      return;
    }
    fat[my_dir] = block2;
    my_dir = block2;
  }
  
  dir_entry *f_dir = (dir_entry *) BLOCK(my_dir);
   
  init_dir_block(block,current_dir);
  init_dir_entry(&f_dir[n_entry%DIR_ENTRIES_PER_BLOCK],TYPE_DIR,nome_dir,0,block);
  
  dir = (dir_entry *) BLOCK(current_dir);
  dir[0].size ++;
  
  return;
}


// cd dir - move o diretório actual para dir
void vfs_cd(char *nome_dir) {
  dir_entry *dir = (dir_entry *) BLOCK(current_dir);
  int n_entry = dir[0].size;
  
  int i,k;
  int mdir = current_dir;
  for(i =0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k == 0 && i != 0){
      mdir = fat[mdir];
      dir = (dir_entry *) BLOCK(mdir);
    }
    if(strcmp(dir[k].name,nome_dir) == 0) break;
  }
  
  if(i == n_entry){
    printf("ERROR(cd: %s not in directory)\n",nome_dir);
    return;
  }
  
  if(dir[k].type != TYPE_DIR){
    printf("ERROR(cd: %s not a directory)\n",nome_dir);
    return;
  }
  
  current_dir = dir[k].first_block;
  
  return;
}

void pwd_aux(int f_b){
  dir_entry *dir = (dir_entry *) BLOCK(f_b);
  int n_entry = dir[0].size;
  
  if(f_b == dir[1].first_block){
    printf("/");
  }
  else {
    pwd_aux(dir[1].first_block);
    
    int i,k;
    int mdir = dir[1].first_block;
    dir = (dir_entry *) BLOCK(mdir);
    n_entry = dir[0].size;
    for(i = 0;i<n_entry;i++){
      k = i%DIR_ENTRIES_PER_BLOCK;
      if(k==0 && i!=0){
        mdir = fat[mdir];
        dir = (dir_entry *) BLOCK(mdir);
      }
      if(dir[k].first_block == f_b)
        break;
    }
    
    printf("%s",dir[k].name);
  }
  
  return;
}

// pwd - escreve o caminho absoluto do diretório actual
void vfs_pwd(void) {
  pwd_aux(current_dir);
  printf("\n");
  return;
}


// rmdir dir - remove o subdiretório dir (se vazio) do diretório actual
void vfs_rmdir(char *nome_dir) {
  dir_entry *dir = (dir_entry *)  BLOCK(current_dir);
  int n_entry = dir[0].size;
  
  int i,k;
  int mdir = current_dir;
  int prev = mdir;
  
  for(i = 0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k == 0 && i != 0){
      prev = mdir;
      mdir = fat[mdir];
      dir = (dir_entry *) BLOCK(mdir);
    }
    if(strcmp(dir[k].name,nome_dir) == 0) break;
  }
  
  if(i == n_entry){
    printf("ERROR(rmdir: %s not in directory)\n",nome_dir);
    return;
  }
  
  if(dir[k].type != TYPE_DIR){
    printf("ERROR(rmdir: %s not a directory)\n",nome_dir);
    return;
  }
  
  if(i<2){
    printf("ERROR(rmdir: %s is a invalid directory ('.' ou '..'))\n",nome_dir);
    return;
  }
  
  dir_entry *d_tmp = (dir_entry *) BLOCK(dir[k].first_block);
  if(d_tmp[0].size > 2){
    printf("ERROR(rmdir: %s is not empty)\n",nome_dir);
    return;
  }
  
  put_free_block(dir[k].first_block);
  
  d_tmp = (dir_entry *) BLOCK(mdir);
  
  if(i != n_entry - 1){
    int i2,k2;
    for(i2 = i;i2<n_entry;i2++){
      k2 = i2%DIR_ENTRIES_PER_BLOCK;
      if(k2 == 0 && i2!=0){
        prev = mdir;
        mdir = fat[mdir];
        d_tmp = (dir_entry *) BLOCK(mdir);
      }
    }
    
    dir[k] = d_tmp[k2];
  }
  
  if((n_entry - 1)%DIR_ENTRIES_PER_BLOCK == 0){
    put_free_block(mdir);
    fat[prev] = -1;
  }
  
  dir = (dir_entry *) BLOCK(current_dir);
  dir[0].size --; 
  
  return;
}


// get fich1 fich2 - copia um ficheiro normal UNIX fich1 para um ficheiro no nosso sistema fich2
void vfs_get(char *nome_orig, char *nome_dest) {
  dir_entry *dir = (dir_entry *) BLOCK(current_dir);
  dir_entry *dir_o = (dir_entry *) BLOCK(current_dir);
  int n_entry = dir[0].size;
  int mdir = current_dir;
  
  if(strlen(nome_dest) > 20){
    printf("ERROR(get: name too long)\n");
    return;
  }
  
  int i,k;
  for(i = 0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k == 0 && i != 0){
      mdir = fat[mdir];
      dir = (dir_entry *) BLOCK(mdir);
    }
    if(strcmp(nome_dest,dir[k].name) == 0) break;
  }
  
  if(i != n_entry){
    printf("ERROR(get: name already exists)\n");
    return;
  }
  
  struct stat my_stat;
  if(lstat(nome_orig, &my_stat) == -1){
    printf("ERROR(get: couldnt found file %s)\n",nome_orig);
    return;
  }
  
  int f_size = my_stat.st_size;
  int req_size = (my_stat.st_size + sb->block_size - 1)/sb->block_size;
  int require_blocks =  (k == DIR_ENTRIES_PER_BLOCK-1) + req_size;
  
  if(require_blocks > sb->n_free_blocks){
    printf("ERROR(get: disk is full)\n");
    return;
  }
  
  dir_o[0].size ++;
  
  int temp;
  if(k == DIR_ENTRIES_PER_BLOCK-1){
    temp = get_free_block();
    fat[mdir] = temp;
    mdir = temp;
  }
  
  k = n_entry % DIR_ENTRIES_PER_BLOCK;
  dir = (dir_entry *) BLOCK(mdir);
  
  int f_b = get_free_block();
  int prev = f_b;
  
  init_dir_entry(&dir[k],TYPE_FILE,nome_dest,f_size,f_b);
  
  int f = open(nome_orig, O_RDONLY);
  char msg[5000];
  
  int n;
  n = read(f,msg,sb->block_size);
  while (n>0){
    strcpy(BLOCK(f_b), msg);
    n = read(f,msg,sb->block_size);
    if(n>0){
      prev = f_b;
      f_b = get_free_block();
      fat[prev] = f_b;
    }
  }
  
  return;
}


// put fich1 fich2 - copia um ficheiro do nosso sistema fich1 para um ficheiro normal UNIX fich2
void vfs_put(char *nome_orig, char *nome_dest) {
  return;
}


// cat fich - escreve para o ecrã o conteúdo do ficheiro fich
void vfs_cat(char *nome_fich) {
  dir_entry *dir =(dir_entry *) BLOCK(current_dir);
  int n_entry = dir[0].size;
  int mdir = current_dir;
  
  int k,i;  
  for(i = 0;i<n_entry;i++){
    k = i%DIR_ENTRIES_PER_BLOCK;
    if(k==0 && i!=0){
      mdir = fat[mdir];
      dir = (dir_entry*) BLOCK(mdir);
    }
    if(strcmp(dir[k].name,nome_fich) == 0) break;
  }
  
  if(i == n_entry){
    printf("ERROR(cat: no file with name '%s')\n",nome_fich);
    return;
  }
  
  if(dir[k].type != TYPE_FILE){
    printf("ERROR(cat: '%s' is not a file)\n",nome_fich);
    return;
  }
  
  int block = dir[k].first_block;
  int size = dir[k].size;
  while(size>0){
    if(size < sb->block_size)
      write(STDOUT_FILENO, BLOCK(block), size);
    else
      write(STDOUT_FILENO, BLOCK(block), sb->block_size);
    size -= sb->block_size;
    block = fat[block]; 
  }
  
  return;
}


// cp fich1 fich2 - copia o ficheiro fich1 para fich2
// cp fich dir - copia o ficheiro fich para o subdiretório dir
void vfs_cp(char *nome_orig, char *nome_dest) {
  return;
}


// mv fich1 fich2 - move o ficheiro fich1 para fich2
// mv fich dir - move o ficheiro fich para o subdiretório dir
void vfs_mv(char *nome_orig, char *nome_dest) {
  return;
}


// rm fich - remove o ficheiro fich
void vfs_rm(char *nome_fich) {
  return;
}
