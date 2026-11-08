#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#define FIELD_MAX 4096
#define USER_MAX 8
#define SUBJECT_MAX 80

typedef struct
{
    char *data; size_t len, cap;
} Buffer;

typedef struct
{
    unsigned long long id;
    char *name;
} MailFile;

static int append(Buffer *b, const char *s, size_t n)
{
    if (n > SIZE_MAX - b->len - 1)
    {
        return -1;
    }

    size_t need = b->len + n + 1;

    if (need > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;

        while (cap < need)
        {
            if (cap > SIZE_MAX / 2)
            {
                cap = need; break;
            }

            cap *= 2;
        }

        char *p = realloc(b->data, cap);

        if (!p)
        {
            return -1;
        }

        b->data = p; b->cap = cap;
    }

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static void clear_buffer(Buffer *b)
{
    free(b->data);
    *b = (Buffer){0};
}


static int read_line(int fd, Buffer *out, size_t max)   //0=success, 1=EOF, -1=error/oversize.
{
    out->len = 0;

    if (out->data)
    {
        out->data[0] = '\0';
    }

    for (;;)
    {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n == 0)
        {
            return 1;
        }

        if (n < 0)
        {
            return -1;
        }

        if (c == '\n')
        {
            if (out->len && out->data[out->len-1] == '\r'){
                out->data[--out->len] = '\0';
            }

            return 0;
        }

        if (max && out->len >= max)
        {
            return -1;
        }

        if (append(out, &c, 1))
        {
            return -1;
        }
    }
}

static int send_all(int fd, const char *s, size_t len)
{
    while (len)
    {
        ssize_t n = send(fd, s, len, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n <= 0)
        {
            return -1;
        }

        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static int reply(int fd, const char *s)
{
    return send_all(fd, s, strlen(s));
}

static int valid_user(const char *s)
{
    size_t n = strlen(s);

    if (!n || n > USER_MAX)
    {
        return 0;
    }

    for (; *s; ++s)
    {
        if (!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9'))
        {
            return 0;
        }
    }
    return 1;
}

static int user_dir(char *dst, size_t cap, const char *root, const char *user)
{
    int n = snprintf(dst, cap, "%s/%s", root, user);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0700) == 0)
    {
        return 0;
    }

    if (errno != EEXIST)
    {
        return -1;
    }

    struct stat st;


    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int file_cmp(const void *a, const void *b)
{
    const MailFile *x=a, *y=b;
    return (x->id > y->id) - (x->id < y->id);
}
static void free_files(MailFile *files, size_t n)
{
    for (size_t i=0;i<n;i++)
    {
        free(files[i].name);
    }

    free(files);
}
static int parse_filename(const char *s, unsigned long long *id)
{
    if (*s < '0' || *s > '9')
    {
        return 0;
    }

    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);

    if (errno || !v || strcmp(end, ".msg") != 0)
    {
        return 0;
    }

    *id = v; return 1;
}

static int list_files(const char *dir, MailFile **result, size_t *count)    //increasing ID order
{
    *result = NULL; *count = 0;
    DIR *d = opendir(dir);

    if (!d)
    {
        return errno == ENOENT ? 0 : -1;
    }

    MailFile *files = NULL;
    size_t n=0, cap=0;
    struct dirent *e;
    int status=0;

    while ((e = readdir(d)))
    {
        unsigned long long id;

        if (!parse_filename(e->d_name, &id))
        {
            continue;
        }

        if (n == cap)
        {
            size_t next = cap ? cap*2 : 8;

            if (next < cap || next > SIZE_MAX/sizeof(*files))
            {
                status=-1;
                break;
            }

            MailFile *p=realloc(files, next*sizeof(*files));

            if (!p)
            {
                status=-1;
                break;
            }

            files=p;
            cap=next;
        }

        files[n].name=strdup(e->d_name);

        if (!files[n].name)
        {
            status=-1;
            break;
        }

        files[n].id=id; n++;
    }

    if (closedir(d))
    {
        status=-1;
    }

    if (status)
    {
        free_files(files,n);
        return -1;
    }

    qsort(files,n,sizeof(*files),file_cmp);
    *result=files; *count=n; return 0;
}

static int path_for(char *dst, size_t cap, const char *dir, const char *name)
{
    int n=snprintf(dst,cap,"%s/%s",dir,name);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int write_all_file(int fd, const char *s, size_t len)
{
    while (len)
    {
        ssize_t n=write(fd,s,len);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n <= 0)
        {
            return -1;
        }

        s+=n; len-=(size_t)n;
    }
    return 0;
}

static int save_mail(const char *root, const char *receiver, const Buffer *mail)
{
    char dir[PATH_MAX], tmp[PATH_MAX], final[PATH_MAX];
    if (user_dir(dir,sizeof(dir),root,receiver) || mkdir_if_missing(dir))
    {
        return -1;
    }

    MailFile *files=NULL; size_t count=0;
    if (list_files(dir,&files,&count))
    {
        return -1;
    }

    unsigned long long id=count ? files[count-1].id : 0;
    free_files(files,count);

    if (id == ULLONG_MAX)
    {
        return -1;
    }

    int n=snprintf(tmp,sizeof(tmp),"%s/.tmpXXXXXX",dir);

    if (n < 0 || (size_t)n >= sizeof(tmp))
    {
        return -1;
    }

    int fd=mkstemp(tmp);

    if (fd < 0)
    {
        return -1;
    }

    int ok=write_all_file(fd,mail->data,mail->len);

    if (ok == 0 && fsync(fd))
    {
        ok=-1;
    }

    if (close(fd))
    {
        ok=-1;
    }

    if (!ok)
    {
        do
        {
            ++id;
            n=snprintf(final,sizeof(final),"%s/%llu.msg",dir,id);
            if (n < 0 || (size_t)n >= sizeof(final)) { ok=-1; break; }
        } while (access(final,F_OK) == 0 && id != ULLONG_MAX);

        if (id == ULLONG_MAX && access(final,F_OK) == 0)
        {
            ok=-1;
        }

        if (!ok && rename(tmp,final))
        {
            ok=-1;
        }
    }

    if (ok)
    {
        unlink(tmp);
    }
    return ok;
}
static int send_subject(int client, const char *path)
{
    FILE *f=fopen(path,"rb");

    if (!f)
    {
        return -1;
    }

    char *line=NULL; size_t cap=0; ssize_t n;

    for (int i=0;i<3;i++)
    {
        n=getline(&line,&cap,f);

        if (n < 0)
        {
            free(line);
            fclose(f);
            return -1;
        }
    }

    int status=send_all(client,line,(size_t)n);

    if (n==0 || line[n-1]!='\n')
    {
        status= status ? status : reply(client,"\n");
    }
    free(line);
    fclose(f);
    return status;
}
static int handle_send(int client, const char *root)
{
    Buffer line={0}, mail={0};
    char receiver[USER_MAX+1]={0};
    int valid=1, status=-1;

    for (int i=0;i<3;i++)
    {
        int r=read_line(client,&line,FIELD_MAX);

        if (r)
        {
            goto done;
        }

        if (i==0 || i==1)
        {
            if (!valid_user(line.data ? line.data : ""))
            {
                valid=0;
            }

            if (i==1 && valid)
            {
                strcpy(receiver,line.data);
            }
        } else if (line.len > SUBJECT_MAX)
        {
            valid=0;
        }

        if (append(&mail,line.data ? line.data : "",line.len) || append(&mail,"\n",1))
        {
            goto done;
        }
    }

    for (;;)
    {
        if (read_line(client,&line,0))
        {
            goto done;
        }
        if (line.len==1 && line.data[0]=='.')
        {
            break;
        }
        if (valid && (append(&mail,line.data ? line.data : "",line.len) || append(&mail,"\n",1)))
        {
            goto done;
        }
    }

    if (valid && append(&mail,".\n",2))
    {
        goto done;
    }

    status=reply(client,valid && save_mail(root,receiver,&mail)==0 ? "OK\n" : "ERR\n");

    done:
        clear_buffer(&line);
        clear_buffer(&mail);
        return status;
}

static int handle_list(int client, const char *root)
{
    Buffer user={0};
    char dir[PATH_MAX];
    MailFile *files=NULL;
    size_t count=0;
    int status=-1;

    if (read_line(client,&user,FIELD_MAX))
    {
        goto done;
    }

    if (valid_user(user.data ? user.data : "") && (user_dir(dir,sizeof(dir),root,user.data) || list_files(dir,&files,&count)))
    {
        goto done;
    }

    char number[64]; int n=snprintf(number,sizeof(number),"%zu\n",count);

    if (send_all(client,number,(size_t)n))
    {
        goto done;
    }

    for (size_t i=0;i<count;i++)
    {
        char path[PATH_MAX];

        if (path_for(path,sizeof(path),dir,files[i].name) || send_subject(client,path))
        {
            goto done;
        }
    }

    status=0;

    done:
        free_files(files,count);
        clear_buffer(&user);
        return status;
}
static int parse_index(const char *s, size_t *out)
{
    if (!s || !*s)
    {
        return -1;
    }

    for (const char *p=s;*p;p++)
    {
        if (*p<'0'||*p>'9')
        {
            return -1;
        }
    }

    errno=0; char *end; unsigned long long v=strtoull(s,&end,10);
    if (errno || *end || v==0 || v>SIZE_MAX)
    {
        return -1;
    }

    *out=(size_t)v; return 0;
}
static int handle_read_del(int client, const char *root, int del)
{
    Buffer user={0}, number={0}; char dir[PATH_MAX], path[PATH_MAX];
    MailFile *files=NULL;
    size_t count=0, index=0;
    int status=-1;

    if (read_line(client,&user,FIELD_MAX) || read_line(client,&number,FIELD_MAX))
    {
        goto done;
    }

    if (!valid_user(user.data ? user.data : "") || parse_index(number.data,&index) || user_dir(dir,sizeof(dir),root,user.data) || list_files(dir,&files,&count) ||
        index>count || path_for(path,sizeof(path),dir,files[index-1].name))
    {
        status=reply(client,"ERR\n"); goto done;
    }

    if (del)
    {
        status=reply(client,unlink(path)==0 ? "OK\n" : "ERR\n");
    }
    else
    {
        int fd=open(path,O_RDONLY);

        if (fd<0)
        {
            status=reply(client,"ERR\n");
            goto done;
        }

        if (reply(client,"OK\n"))
        {
            close(fd); goto done;
        }

        char buf[8192]; ssize_t n;
        status=0;

        while ((n=read(fd,buf,sizeof(buf))) != 0)
        {
            if (n<0 && errno==EINTR)
            {
                continue;
            }

            if (n<0 || send_all(client,buf,(size_t)n))
            {
                status=-1;
                break;
            }
        }

        close(fd);
    }
    done:
        free_files(files,count);
        clear_buffer(&user);
        clear_buffer(&number);
        return status;
}

static void handle_client(int client, const char *root)
{
    Buffer command={0};
    while (read_line(client,&command,FIELD_MAX)==0)
    {
        const char *s=command.data ? command.data : "";
        int result;

        if (!strcmp(s,"SEND"))
        {
            result=handle_send(client,root);
        }
        else if (!strcmp(s,"LIST"))
        {
            result=handle_list(client,root);
        }
        else if (!strcmp(s,"READ"))
        {
            result=handle_read_del(client,root,0);
        }
        else if (!strcmp(s,"DEL"))
        {
            result=handle_read_del(client,root,1);
        }
        else if (!strcmp(s,"QUIT"))
        {
            break;
        }
        else
        {
            result=reply(client,"ERR\n");
        }
        if (result)
        {
            break;
        }
    }
    clear_buffer(&command);
}

int main(int argc, char **argv) {
    if (argc!=3)
    {
        fprintf(stderr,"Usage: %s <port> <mail-spool-directoryname>\n",argv[0]); return EXIT_FAILURE;
    }

    errno=0; char *end;
    long port=strtol(argv[1],&end,10);

    if (errno || !*argv[1] || *end || port<1 || port>65535)
    {
        fprintf(stderr,"Invalid port (1-65535)\n");
        return EXIT_FAILURE;
    }

    if (mkdir_if_missing(argv[2]))
    {
        perror("mail spool directory");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE,SIG_IGN);
    int server=socket(AF_INET,SOCK_STREAM,0);

    if (server<0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    int yes=1;
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons((uint16_t)port);

    if (bind(server,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(server,5)<0) {
        perror("bind/listen");
        close(server);
        return EXIT_FAILURE;
    }

    printf("TW-Mailer C server listening on port %ld\n",port);
    fflush(stdout);

    for (;;)
    {
        int client=accept(server,NULL,NULL);

        if (client<0)
        {
            if (errno==EINTR)
            {
                continue;
            }

            perror("accept");
            continue;
        }

        handle_client(client,argv[2]);
        close(client);
    }
}