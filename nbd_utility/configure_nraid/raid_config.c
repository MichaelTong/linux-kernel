#include <libssh/libssh.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

struct host_user
{
    char *host;
    int port;
    char *username;
    char *passwd;
};

int verify_knownhost(ssh_session);
int ssh_exec_sigcmd(ssh_session, char*);
int read_config(char*, struct host_user **);
int connect_ssh(ssh_session *, char *, int , char *, char *);


int main(int argc, char *argv[])
{
    int num, i=0, l=0;
    ssh_session ss;
    struct host_user *hus=NULL;
    char cmd[100];
    bool limp = true;

    if(argc<=1)
        exit(-1);

    if(argv[1][0]=='l')
    {
        limp = true;
        l = 2;
    }
    else if(argv[1][0]=='s')
        limp = false;
    else if(isdigit(argv[1][0]))
    {
        limp = true;
        l = 1;
    }
    else
        exit(-1);

    num = read_config("nraid.config", &hus);
    while(i<num)
    {
        //printf("%s, %d, %s, %s\n", hus[i].host, hus[i].port, hus[i].username, hus[i].passwd);
        if(connect_ssh(&ss, hus[i].host, hus[i].port, hus[i].username, hus[i].passwd))
        {
            exit(-1);
        }
        if(limp)
        {
            if(l < argc)
                sprintf(cmd, "sudo tc qdisc add dev eth0 root netem delay %sms", argv[l]);
            else
                sprintf(cmd, "sudo tc qdisc del dev eth0 root");
        }
        else
            sprintf(cmd, "sudo tc qdisc del dev eth0 root");
        if(ssh_exec_sigcmd(ss, cmd)!=SSH_OK)
        {
            fprintf(stderr, "Command Execution failed on host: %s, port: %d, user: %s\n",
                    hus[i].host, hus[i].port, hus[i].username);
        }
        else
        {
            fprintf(stdout, "Command Execution succeeded on host: %s, port: %d, user: %s\n",
                    hus[i].host, hus[i].port, hus[i].username);
        }

        ssh_disconnect(ss);
        ssh_free(ss);
        i++;
        l++;
    }

    return 0;
}

int connect_ssh(ssh_session *session, char *host, int port, char *username, char *passwd)
{
    int rc;
    *session = ssh_new();
    if(*session == NULL)
        return -1;
    ssh_options_set(*session, SSH_OPTIONS_HOST, host);
    //ssh_options_set(ss, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(*session, SSH_OPTIONS_PORT, &port);
    rc = ssh_connect(*session);
    if(rc != SSH_OK)
    {
        fprintf(stderr, "Error connecting to %s: %s\n",
                host, ssh_get_error(*session));
    }

    if(verify_knownhost(*session))
    {
        ssh_disconnect(*session);
        ssh_free(*session);
        return -1;
    }

    rc = ssh_userauth_password(*session, username, passwd);
    if (rc != SSH_AUTH_SUCCESS)
    {
        fprintf(stderr, "Error authenticating with password: %s\n",
                ssh_get_error(*session));
        ssh_disconnect(*session);
        ssh_free(*session);
        return -1;
    }
    return 0;
}

int read_config(char* file, struct host_user **hus)
{
    FILE *fp = fopen(file, "r");
    int num, i=0;
    char temp[100];
    char host[100]="";
    int port;
    char username[100]="";
    char passwd[100]="";

    if(fp==NULL)
    {
        fprintf(stderr, "Cannot open config file: %s", file);
        exit(-1);
    }


    fgets(temp, 100, fp);
    num = atoi(strstr(temp, "=")+1);
    *hus = (struct host_user *)malloc(num*sizeof(struct host_user));

    while(i<num)
    {
        fgets(temp, 100, fp);
        fgets(temp, 100, fp);
        strcpy(host, strstr(temp, "=")+1);
        host[strlen(host)-1]=0;
        fgets(temp, 100, fp);
        port = atoi(strstr(temp, "=")+1);
        fgets(temp, 100, fp);
        strcpy(username, strstr(temp, "=")+1);
        username[strlen(username)-1]=0;
        fgets(temp, 100, fp);
        strcpy(passwd, strstr(temp, "=")+1);
        passwd[strlen(passwd)-1]=0;

        (*hus)[i].host=(char *)malloc(strlen(host)*sizeof(char));
        strcpy((*hus)[i].host, host);

        (*hus)[i].port = port;

        (*hus)[i].username=(char *)malloc(strlen(username)*sizeof(char));
        strcpy((*hus)[i].username, username);

        (*hus)[i].passwd=(char *)malloc(strlen(passwd)*sizeof(char));
        strcpy((*hus)[i].passwd, passwd);

        i++;

    }
    fclose(fp);
    return num;
}

int ssh_exec_sigcmd(ssh_session session, char* cmd)
{
    ssh_channel ch;
    int rc;
    char buffer[256];
    unsigned int nbytes;

    ch = ssh_channel_new(session);
    if(ch == NULL)
        return SSH_ERROR;

    rc = ssh_channel_open_session(ch);
    if(rc != SSH_OK)
    {
        ssh_channel_free(ch);
        return rc;
    }

    rc = ssh_channel_request_exec(ch, cmd);
    if(rc!=SSH_OK)
    {
        ssh_channel_close(ch);
        ssh_channel_free(ch);
        return rc;
    }

    nbytes = ssh_channel_read(ch, buffer, sizeof(buffer), 0);
    while (nbytes > 0)
    {
        if (write(1, buffer, nbytes) != nbytes)
        {
            ssh_channel_close(ch);
            ssh_channel_free(ch);
            return SSH_ERROR;
        }
        nbytes = ssh_channel_read(ch, buffer, sizeof(buffer), 0);
    }

    if (nbytes < 0)
    {
        ssh_channel_close(ch);
        ssh_channel_free(ch);
        return SSH_ERROR;
    }
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
    return SSH_OK;
}

int verify_knownhost(ssh_session session)
{
    int state, hlen;
    unsigned char *hash = NULL;
    char *hexa;
    char buf[10];
    state = ssh_is_server_known(session);
    hlen = ssh_get_pubkey_hash(session, &hash);
    if (hlen < 0)
        return -1;
    switch (state)
    {
    case SSH_SERVER_KNOWN_OK:
        break; /* ok */
    case SSH_SERVER_KNOWN_CHANGED:
        fprintf(stderr, "Host key for server changed: it is now:\n");
        ssh_print_hexa("Public key hash", hash, hlen);
        fprintf(stderr, "For security reasons, connection will be stopped\n");
        free(hash);
        return -1;
    case SSH_SERVER_FOUND_OTHER:
        fprintf(stderr, "The host key for this server was not found but an other"
                "type of key exists.\n");
        fprintf(stderr, "An attacker might change the default server key to"
                "confuse your client into thinking the key does not exist\n");
        free(hash);
        return -1;
    case SSH_SERVER_FILE_NOT_FOUND:
        fprintf(stderr, "Could not find known host file.\n");
        fprintf(stderr, "If you accept the host key here, the file will be"
                "automatically created.\n");
    /* fallback to SSH_SERVER_NOT_KNOWN behavior */
    case SSH_SERVER_NOT_KNOWN:
        hexa = ssh_get_hexa(hash, hlen);
        fprintf(stderr,"The server is unknown. Do you trust the host key?\n");
        fprintf(stderr, "Public key hash: %s\n", hexa);
        free(hexa);
        if (fgets(buf, sizeof(buf), stdin) == NULL)
        {
            free(hash);
            return -1;
        }
        if (strncasecmp(buf, "yes", 3) != 0)
        {
            free(hash);
            return -1;
        }
        if (ssh_write_knownhost(session) < 0)
        {
            fprintf(stderr, "Error %s\n", strerror(errno));
            free(hash);
            return -1;
        }
        break;
    case SSH_SERVER_ERROR:
        fprintf(stderr, "Error %s", ssh_get_error(session));
        free(hash);
        return -1;
    }
    free(hash);
    return 0;
}
