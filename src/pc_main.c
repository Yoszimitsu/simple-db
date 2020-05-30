#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <mqueue.h>
#include "SQL_parser.h"
#include "table.h"
#include "pc_main.h"
#include "query_mq.h"
#include "util.h"

static mqd_t query_mq;

static void open_query_mq()
{
	/* open the mail queue */
	struct mq_attr attr = QUERY_QUEUE_ATTR;
	query_mq = mq_open(QUERY_QUEUE_NAME, O_CREAT | O_WRONLY, QUEUE_PERMS, &attr);
	CHECK((mqd_t)-1 != query_mq);
}

static void childSignalHandler(int s)
{

	int status;
	int code;
	int signum;
	pid_t pid;

	if ((pid = waitpid(-1, &status, WUNTRACED | WCONTINUED)) != -1)
	{
		// Status is a multifield value with this information:
		//      Event: Type of state change (EXITED|TERMINATED BY SIGNAL|STOPPED|CONTINUED)
		//      Event information: Number of the signal which terminated/stopped the child
		//      Exit code for exited processes.

		if (WIFEXITED(status))
		{
			code = WEXITSTATUS(status);
			fprintf(stderr, "parent: child %d terminated with exit(%d)\n", pid, code);
		}
		if (WIFSIGNALED(status))
		{
			signum = WTERMSIG(status);
			fprintf(stderr, "parent: child %d kill by signal %d\n", pid, signum);
		}
		if (WIFSTOPPED(status))
		{
			signum = WSTOPSIG(status);
			fprintf(stderr, "parent: child %d stopped by signal %d\n", pid, signum);
		}
		if (WIFCONTINUED(status))
		{
			fprintf(stderr, "parent: child %d continued\n", pid);
		}
	}
}

static ssize_t readLine(int fd, void *buffer, size_t n)
{
	ssize_t numRead; /* # of bytes fetched by last read() */
	size_t totRead;	 /* Total bytes read so far */
	char *buf;
	char ch;

	if (n <= 0 || buffer == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	buf = buffer; /* No pointer arithmetic on "void *" */

	totRead = 0;
	for (;;)
	{
		numRead = read(fd, &ch, 1);

		if (numRead == -1)
		{
			if (errno == EINTR) /* Interrupted --> restart read() */
				continue;
			else
				return -1; /* Some other error */
		}
		else if (numRead == 0)
		{					  /* EOF */
			if (totRead == 0) /* No bytes read; return 0 */
				return 0;
			else /* Some bytes read; add '\0' */
				break;
		}
		else
		{ /* 'numRead' must be 1 if we get here */
			if (totRead < n - 1)
			{ /* Discard > (n - 1) bytes */
				totRead++;
				*buf++ = ch;
			}

			if (ch == '\n')
			{
				*(buf - 1) = '\0';
				totRead--;
				break;
			}
		}
	}

	*buf = '\0';
	return totRead;
}

static int send_to_client(int clientFd, char *message)
{
	int send_res = send(clientFd, message, strlen(message), 0);
	if (send_res == -1)
	{
		perror("send");
	}

	return send_res;
}

static void handle_result(int client_fd)
{
	// read result from transaction manager and pass it to the client socket
	char result[RESULT_MSG_SIZE];

	char q_name[sizeof(RESULTS_QUEUE_NAME) + 10];
	sprintf(q_name, "%s.%d", RESULTS_QUEUE_NAME, getpid());

    struct mq_attr attr = RESULT_QUEUE_ATTR;
	mqd_t res_mq = mq_open(q_name, O_CREAT | O_RDONLY, QUEUE_PERMS, &attr);
	CHECK((mqd_t)-1 != res_mq);

	mq_receive(res_mq, result, RESULT_MSG_SIZE, NULL);
	printf("Query results:%s\n", result);

	int send_res = send(client_fd, result, strlen(result), 0);
	CHECK(send_res != -1);

	mq_close(res_mq);
	mq_unlink(q_name);
}

/* read 1 line from client and handle the query */
static void handle_client_query(int client_fd)
{
	char sqlCommand[MAX_QUERY_LEN];
	int recv_res = readLine(client_fd, sqlCommand, MAX_QUERY_LEN);
	if (recv_res == -1)
	{
		perror("Connection closed.");
		close(client_fd);
		exit(EXIT_SUCCESS);
	}
	else if (recv_res == 0)
	{
		/* empty line ends session */
		printf("Client ended connection.\n");
		close(client_fd);
		exit(EXIT_SUCCESS);
	}

	printf("Client sent %d bytes of data.\n", recv_res);
	printf("SQL query: %s\n", sqlCommand);
	query_msg_t query_msg;
	query_msg.pid = getpid();

	if (parse_SQL(sqlCommand, &query_msg.query))
	{
		// send_to_client(client_fd, "SQL query successfully parsed!\n");
		mq_send(query_mq, (const char *)&query_msg, QUERY_MSG_SIZE, 0);

		// wait for a query result from transaction_mg and pass it to client socket
		handle_result(client_fd);

	}
	else
	{
		printf("Invalid SQL query!\n");
		send_to_client(client_fd, "Invalid SQL query!\n");
	}
}

int protocol_engine()
{

	struct sockaddr_in server_addr;

	memset(&server_addr, 0, sizeof(struct sockaddr_in)); //clear structure

	int socket_res = socket(PF_INET, SOCK_STREAM, 0);
	if (socket_res == -1)
	{
		perror("socket");
	}

	server_addr.sin_family = AF_INET; // address family AF_INET (IPv4)
	server_addr.sin_port = htons(MYPORT);
	server_addr.sin_addr.s_addr = INADDR_ANY; // set localhost IP (0.0.0.0)

	char str[INET_ADDRSTRLEN];
	printf("Starting protocol_engine server on %s:%d\n", inet_ntop(server_addr.sin_family, &server_addr.sin_addr.s_addr, str, INET_ADDRSTRLEN), MYPORT);

	/*
	 * The error may mean that port is still ocupied in kernel. This code solve the problem.
	 */
	int dummy = 1;
	if (setsockopt(socket_res, SOL_SOCKET, SO_REUSEADDR, &dummy, sizeof(int)) == -1)
	{
		perror("setsockopt");
		exit(1);
	}

	/*
     * bind() assigns the address specified by server_addr to the socket referred to by the file descriptor(socket_res).
	*/
	int bind_res = bind(socket_res, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
	if (bind_res == -1)
	{
		perror("bind");
		exit(EXIT_FAILURE);
	}

	int listen_res = listen(socket_res, BACKLOG);
	if (listen_res == -1)
	{
		perror("listen");
		exit(EXIT_FAILURE);
	}

	pid_t pid;

	// register signal handler for incoming child signals
	signal(SIGCHLD, &childSignalHandler);

	/* Accept client connection and delegate its handling to child process */
	while (1)
	{
		struct sockaddr_in client_addr;
		socklen_t addr_len;
		// create a new client socket
		int client_fd = accept(socket_res, (struct sockaddr *)&client_addr, &addr_len);
		if (client_fd == -1)
		{
			perror("Error when creating client socket.");
			continue;
		}

		/*
		 * This is the parent process before forking. Every change done is this section
		 * will affect both the parent and the child (inherited).
		 */

		pid = fork();
		/* Errors in fork mean that there is no child process. Parent is alone. */
		if (pid == -1)
		{
			fprintf(stderr, "parent: error in fork\n");
			exit(EXIT_FAILURE);
		}

		/* Fork has succeeded. Sentences after fork are being executed both by parent
		 * and child but in their respective process contexts (isolated from each other).
		 */

		if (pid == 0)
		{
			// <POST-FORK CHILD ONLY CODE HERE>

			close(socket_res); //In child process we're using only client_fd, so socket_res can be closed

			char clientIP[16];
			inet_ntop(AF_INET, &client_addr.sin_addr, clientIP, sizeof(clientIP));
			unsigned int clientPort = ntohs(client_addr.sin_port);

			printf("Incoming connection from %s:%d\n", clientIP, clientPort);
			open_query_mq();

			for (;;)
			{
				handle_client_query(client_fd);
			}
		}
		else
		{
			// <POST-FORK PARENT ONLY CODE HERE>
			close(client_fd); // Parent process don't need client socket
		}
	}
	return (EXIT_SUCCESS);
}
