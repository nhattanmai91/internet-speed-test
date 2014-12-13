/**
 * Client: Angela Mendez
 * Bidder: Tan M. (nhattanmai91@gmail.com)
 * Date: 13-Dec-2014 (01:50 pm)
 * Status: on-going 
 * Version: 3
 */

/**
 * USAGE:
 * Compile: make
 * Run: 
 *     ./main
 */

/* C99 */
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

/* POSIX */
#include <poll.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#if !defined(NO_SCHED_FIFO)
#include <sched.h>
#endif

#define DEFAULT_WORKERS 1

#define DEFAULT_HOST "speedtest.mtel.bg"
#define DEFAULT_DL_RESOURCE "/speedtest/random4000x4000.jpg"
//#define DL_RESOURCE "/speedtest/random500x500.jpg"
#define DEFAULT_UL_RESOURCE "/speedtest/upload.php"
#define DEFAULT_UL_SIZE ((size_t)(3 * 1024 * 1024))

#define STATE_NOT_CONNECTED           0
#define STATE_CONNECTING              1
#define STATE_SENDING_REQUEST_HEADER  2
#define STATE_SENDING_REQUEST_BODY    3
#define STATE_READING_REPLY_HEADER    4
#define STATE_READING_REPLY_BODY      5
#define STATE_ERROR                  -1

#define ABOUT "Internet Speed Test."

#define MAX_UL 300000
#define MAX_DL 300000

#define LOGLVL_NO      -1
#define LOGLVL_FORCE    0
#define LOGLVL_ERROR    1
#define LOGLVL_WARNING  2
#define LOGLVL_INFO     3
#define LOGLVL_DEBUG1   4
#define LOGLVL_DEBUG2   5

#define LOGLVL_DEFAULT_MAX LOGLVL_WARNING

/* OpenBSD */
#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

//++++++++++++++++++++++++++prototype++++++++++++++++++++++++++++
typedef int (* work_fn)(void * ctx, short revents, int * fd_ptr, short * events_ptr);
typedef void (* cleanup_fn)(void * ctx);
int worker(void * ctx, short revents, int * fd_ptr, short * events_ptr);
unsigned long time_ms();
int test();
uint32_t resolve_host(const char * hostname);
bool
create_worker(int worker_no, const char * type, uint32_t ip, const char * hostname, void ** ctx, work_fn * work, cleanup_fn * cleanup);
void log_msg(int level, const char * format, ...) __attribute__((format(printf, 2, 3)));

struct connection
{
  int no;
  bool upload;
  const char * host;
  int state;
  int socket;
  uint32_t ip;                  /* network byte order */
  size_t offset;
  size_t size;
  char buffer[1024 * 1024];
};

//++++++++++++++++++++++++global variables+++++++++++++++++++++++
static int g_log_max = LOGLVL_DEFAULT_MAX;
static int g_progress = 0;
static size_t g_workers = DEFAULT_WORKERS;
static const char * g_dl_resource = DEFAULT_DL_RESOURCE;
static const char * g_ul_resource = DEFAULT_UL_RESOURCE;
static size_t g_ul_size = DEFAULT_UL_SIZE;

static int is_dl_done=0, is_ul_done=0;
unsigned long int total_get=0;
unsigned long int total_put=0;
unsigned long start_dl_ms=0, end_dl_ms=0, dl_time=0;
unsigned long start_ul_ms=0, end_ul_ms=0, ul_time=0;
float dl_speed = 0, ul_speed = 0;

void log_msg(int level, const char * format, ...)
{
  va_list ap;

  if (level > g_log_max) return;

  va_start(ap, format);
  vfprintf(level > 0 ? stdout : stderr, format, ap);
  va_end(ap);
}

#define connection_ptr ((struct connection *)ctx)

#define LOG_MSG_(level, format, ...)             \
  log_msg(level, format "\n", ##__VA_ARGS__)
#define LOG_WORKER(level, no, format, ...)                \
  log_msg(level, "[%d] " format "\n", (int)(no), ##__VA_ARGS__)
#define LOG_WORKER_(level, format, ...)                         \
  LOG_WORKER(level, connection_ptr->no, format, ##__VA_ARGS__)

#define LFRC( format, ...) LOG_MSG(LOGLVL_FORCE,   format, ##__VA_ARGS__)
#define LERR( format, ...) LOG_MSG(LOGLVL_ERROR,   format, ##__VA_ARGS__)
#define LWRN( format, ...) LOG_MSG(LOGLVL_WARNING, format, ##__VA_ARGS__)
#define LINF( format, ...) LOG_MSG(LOGLVL_INFO,    format, ##__VA_ARGS__)
#define LDBG1(format, ...) LOG_MSG(LOGLVL_DEBUG1,  format, ##__VA_ARGS__)
#define LDBG2(format, ...) LOG_MSG(LOGLVL_DEBUG2,  format, ##__VA_ARGS__)

#define LOG_MSG LOG_WORKER_

//+++++++++++++++++++++++++++++main++++++++++++++++++++++++++++++
int main(){
  int ret1=1, ret2=1;
  do{
    ret1=test(0); //download
    ret2=test(1); //upload
  }while(1==ret1 || 1==ret2); //run test again if failed
  
  return 0;
}

int worker(void * ctx, short revents, int * fd_ptr, short * events_ptr)
{
  int ret, val;
  struct sockaddr_in sin;
  socklen_t len;
  ssize_t sret;
  size_t i;
  const char * ptr;
  char size_str[100];
  size_t size;

  LDBG2("state=%d", connection_ptr->state);

  switch (connection_ptr->state)
  {
  case STATE_NOT_CONNECTED:
    goto connect;
  case STATE_CONNECTING:
    assert((revents & POLLOUT) == POLLOUT);
    goto async_connect_done;
  case STATE_SENDING_REQUEST_HEADER:
  case STATE_SENDING_REQUEST_BODY:
    assert((revents & POLLOUT) == POLLOUT);
    if ((revents & (POLLERR | POLLHUP)) != 0)
    {
      LERR("async send fd error. revents=%#hx", revents);

      len = sizeof(val);
      ret = getsockopt(connection_ptr->socket, SOL_SOCKET, SO_ERROR, &val, &len);
      if (ret == -1)
      {
        LERR("getsockopt() failed to get socket send error. %d (%s)", errno, strerror(errno));
      }
      else
      {
        LERR("async send() error %d (%s)", val, strerror(val));
      }
      goto error;
    }
    goto send_request_continue;
  case STATE_READING_REPLY_HEADER:
    assert((revents & POLLIN) == POLLIN);
    if ((revents & (POLLERR | POLLHUP)) != 0)
    {
      LERR("async reply header recv fd error. revents=%#hx", revents);

      len = sizeof(val);
      ret = getsockopt(connection_ptr->socket, SOL_SOCKET, SO_ERROR, &val, &len);
      if (ret == -1)
      {
        LERR("getsockopt() failed to get socket recv error. %d (%s)", errno, strerror(errno));
      }
      else
      {
        LERR("async recv() error %d (%s)", val, strerror(val));
      }
      goto error;
    }
    goto read_reply_header;
  case STATE_READING_REPLY_BODY:
    assert((revents & POLLIN) == POLLIN);
    if ((revents & (POLLERR | POLLHUP)) != 0)
    {
      LERR("async reply body recv fd error. revents=%#hx", revents);

      len = sizeof(val);
      ret = getsockopt(connection_ptr->socket, SOL_SOCKET, SO_ERROR, &val, &len);
      if (ret == -1)
      {
        LERR("getsockopt() failed to get socket recv error. %d (%s)", errno, strerror(errno));
      }
      else
      {
        LERR("async recv() error %d (%s)", val, strerror(val));
      }
      goto error;
    }
    goto read_reply_body;
  default:
    assert(false);
    goto error;
  }

  assert(false);

connect:
  connection_ptr->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (connection_ptr->socket == -1)
  {
    LERR("socket() failed. %d (%s)", errno, strerror(errno));
    goto error;
  }

  ret = fcntl(connection_ptr->socket, F_SETFL, O_NONBLOCK);
  if (ret == -1)
  {
    LERR("fcntl() failed to set socket non-blocking mode. %d (%s)", errno, strerror(errno));
    goto error;
  }


  sin.sin_family = AF_INET;
  sin.sin_port = htons(80);
  sin.sin_addr.s_addr = connection_ptr->ip;

  ret = connect(connection_ptr->socket, (struct sockaddr *)&sin, sizeof(struct sockaddr_in));
  if (ret == -1)
  {
    if (errno == EINPROGRESS)
    {
      connection_ptr->state = STATE_CONNECTING;
      *fd_ptr = connection_ptr->socket;
      *events_ptr = POLLOUT;
      return 1;
    }

    LERR("connect() failed. %d (%s)", errno, strerror(errno));
    goto error;
  }

  LINF("connect complete.");
  goto send_request;

async_connect_done:
  if ((revents & (POLLERR | POLLHUP)) != 0)
  {
    LERR("async connect failed. revents=%#hx", revents);
  }

  len = sizeof(val);
  ret = getsockopt(connection_ptr->socket, SOL_SOCKET, SO_ERROR, &val, &len);
  if (ret == -1)
  {
    LERR("getsockopt() failed to get socket connect error. %d (%s)", errno, strerror(errno));
    goto error;
  }
  if (val != 0)
  {
    LERR("async connect() failed. %d (%s)", val, strerror(val));
    goto error;
  }

  LINF("async connect complete.");

send_request:
  LINF("sending request header...");

  if (connection_ptr->upload)
  {
    snprintf(size_str, sizeof(size_str), "%zu", g_ul_size);
  }

  ret = snprintf(
    connection_ptr->buffer,
    sizeof(connection_ptr->buffer),
    "%s %s HTTP/1.1\r\n"
    "User-Agent: netspeed/0.0\r\n"
    "Accept: */*\r\n"
    "Host: %s\r\n"
    "%s%s%s"
    "\r\n",
    connection_ptr->upload ? "POST" : "GET",
    connection_ptr->upload ? g_ul_resource : g_dl_resource,
    connection_ptr->host,
    connection_ptr->upload ? "Content-Length: " : "",
    connection_ptr->upload ? size_str : "",
    connection_ptr->upload ? "\r\n" : "");
  if (ret < -1 || ret >= (int)sizeof(connection_ptr->buffer))
  {
    LERR("snprintf() failed compose request. %d", ret);
    goto error;
  }

  LDBG1("request-header:\n%s", connection_ptr->buffer);

  connection_ptr->state = STATE_SENDING_REQUEST_HEADER;
  connection_ptr->offset = 0;
  connection_ptr->size = (size_t)ret;

send_request_continue:
  while (connection_ptr->size > 0)
  {
    if (connection_ptr->state == STATE_SENDING_REQUEST_BODY &&
        connection_ptr->size >= sizeof(connection_ptr->buffer))
    {
      size = sizeof(connection_ptr->buffer);
    }
    else
    {
      size = connection_ptr->size;
    }

    sret = send(
      connection_ptr->socket,
      connection_ptr->buffer + connection_ptr->offset,
      size,
      MSG_NOSIGNAL);
    if (sret == -1)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        *fd_ptr = connection_ptr->socket;
        *events_ptr = POLLOUT;
        return 1;
      }

      LERR("send() failed. %d (%s)", errno, strerror(errno));
      goto error;
    }
//++++++++++++++++++++++++upload measure+++++++++++++++++++++++++
    if(total_put<1){
      start_ul_ms = time_ms();
    }
    total_put+=sret;
//    printf(">Total put: %ld\n", total_put);
    if(total_put>=MAX_UL){
      end_ul_ms=time_ms();
      ul_time = end_ul_ms-start_ul_ms;
      ul_speed=(float)total_put/(float)ul_time;
//      printf("Time: %ld (ms)\n", ul_time); 
//      printf("Uploaded: %ld (byte)\n", total_put);
      printf("Speed Upload: %.2f (KB/s)\n", ul_speed); 
      is_ul_done=1;
    }
    //end measure time
    if (connection_ptr->state == STATE_SENDING_REQUEST_HEADER)
    {
      connection_ptr->offset += sret;
    }

    connection_ptr->size -= sret;
  }

  if (connection_ptr->state == STATE_SENDING_REQUEST_HEADER)
  {
    LINF("request header sent");

    if (connection_ptr->upload)
    {
      connection_ptr->state = STATE_SENDING_REQUEST_BODY;
      connection_ptr->offset = 0;
      connection_ptr->size = g_ul_size;
      LINF("sending request body...");
      goto send_request_continue;
    }
  }
  else
  {
    LINF("request body sent");
  }

  connection_ptr->state = STATE_READING_REPLY_HEADER;
  connection_ptr->offset = 0;   /* parsed size */
  connection_ptr->size = 0;     /* read size */

read_reply_header:
  if (connection_ptr->size >= sizeof(connection_ptr->buffer))
  {
    LERR("HTTP reply header too big");
    goto error;
  }

  sret = recv(
    connection_ptr->socket,
    connection_ptr->buffer + connection_ptr->size,
    sizeof(connection_ptr->buffer) - connection_ptr->size,
    MSG_NOSIGNAL);
  if (sret == -1)
  {
    if (errno == EINTR)
    {
      goto read_reply_header;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      *fd_ptr = connection_ptr->socket;
      *events_ptr = POLLIN;
      return 1;
    }

    LERR("recv() failed. %d (%s)", errno, strerror(errno));
    goto error;
  }

  connection_ptr->size += sret;

  for (i = connection_ptr->offset; i + 3 < connection_ptr->size; i++)
  {
    if (connection_ptr->buffer[i    ] == '\r' &&
        connection_ptr->buffer[i + 1] == '\n' &&
        connection_ptr->buffer[i + 2] == '\r' &&
        connection_ptr->buffer[i + 3] == '\n')
    {
      connection_ptr->offset = i + 4;
      LINF("header size is %zu bytes", connection_ptr->offset);
      for (i = 0; i < connection_ptr->offset; i++)
      {
        if ((signed char)connection_ptr->buffer[i] < 0)
        {
          LERR("invalid char in HTTP reply header");
          goto error;
        }

        connection_ptr->buffer[i] = tolower(connection_ptr->buffer[i]);
      }

      connection_ptr->buffer[connection_ptr->offset] = 0;
      LDBG1("reply-header:\n%s", connection_ptr->buffer);

      /* calculate the size of body bytes we already read */
      i = connection_ptr->size - connection_ptr->offset;

      ptr = strstr(connection_ptr->buffer, "content-length");
      if (ptr == NULL)
      {
        goto unknown_size;
      }

      ptr += sizeof("content-length") - 1;

      while (*ptr == ' ') ptr++;

      if (*ptr != ':')
      {
        goto unknown_size;
      }
      ptr++;

      while (*ptr == ' ') ptr++;

      val = atoi(ptr);

      if (val > 0)
      {
        LINF("total body size is %d bytes", val);

        if ((size_t)val < i)
        {
          LERR("body bigger than announced");
          goto error;
        }

        /* substract the already received body bytes */
        connection_ptr->size = (size_t)val - i;
      }
      else
      {
      unknown_size:
       /* server didnt provide body size,
           assume body end will be marked by connection close */
        LWRN("unknown body size");
        goto error;
        connection_ptr->size = SIZE_MAX;
      }
      
      connection_ptr->state = STATE_READING_REPLY_BODY;
      connection_ptr->offset = i;
      goto read_reply_body;
    }
  }

  if (i >= 4)
  {
    /* next time don't parse the bytes already parsed */
    connection_ptr->offset = i - 4;
  }

  goto read_reply_header;

read_reply_body:
  while (connection_ptr->size > 0)
  {
    sret = recv(
      connection_ptr->socket,
      connection_ptr->buffer,
      sizeof(connection_ptr->buffer),
      MSG_NOSIGNAL);
    if (sret == -1)
    {
      if (errno == EINTR)
      {
        goto read_reply_header;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        *fd_ptr = connection_ptr->socket;
        *events_ptr = POLLIN;
        return 1;
      }

      LERR("recv() failed. %d (%s)", errno, strerror(errno));
      goto error;
    }
//+++++++++++++++++++++++download measure++++++++++++++++++++++++
    if(total_get<1){
      start_dl_ms = time_ms();
    }
    total_get+=sret;
//    printf(">Total get: %ld\n", total_get); 
    if(total_get>=MAX_DL){
      end_dl_ms=time_ms();
      dl_time = end_dl_ms-start_dl_ms;
      dl_speed=(float)total_get/(float)dl_time;
//      printf("Time: %ld (ms)\n", dl_time); 
//      printf("Downloaded: %ld (byte)\n", total_get);
      printf("Speed Download: %.2f (KB/s)\n", dl_speed); 
      is_dl_done=1;
    }
    //end Tan code
    connection_ptr->size -= sret;
    connection_ptr->offset += sret;
    if (g_progress > 0)
    {
      if (g_progress == 1)
      {
        printf(".");
      }
      else
      {
        printf("(%zd)", sret);
      }

      fflush(stdout);
    }
  }

  LINF("%zu body bytes read", connection_ptr->offset);
  goto send_request;
  //return 0;                     /* done */

error:
  connection_ptr->state = STATE_ERROR;
  return -1;
}

void connection_cleanup(void * ctx)
{
  if (connection_ptr->socket != -1)
  {
    LINF("closing socket...");
    close(connection_ptr->socket);
  }
}

#undef connection_ptr
#undef LOG_MSG
#define LOG_MSG LOG_MSG_

uint32_t resolve_host(const char * hostname)
{
  struct hostent * he_ptr;

  he_ptr = gethostbyname(hostname);
  if (he_ptr == NULL)
  {
    LERR("Cannot resolve \"%s\". h_errno is %d", hostname, h_errno);
    return 0;
  }

  return *(uint32_t *)(he_ptr->h_addr);
}

bool
create_worker(
  int worker_no,
  const char * type,
  uint32_t ip,
  const char * hostname,
  void ** ctx,
  work_fn * work,
  cleanup_fn * cleanup)
{
  struct connection * connection_ptr;
  bool upload;

  if (strcmp(type, "d") == 0)
  {
    upload = false;
  }
  else if (strcmp(type, "u") == 0)
  {
    upload = true;
  }
  else
  {
    LOG_WORKER(LOGLVL_ERROR, worker_no, "unknown type \"%s\".", type);
    return false;
  }

  LOG_WORKER(LOGLVL_INFO, worker_no, "connecting to %s for %s", hostname, upload ? "uploading" : "downloading");

  connection_ptr = malloc(sizeof(struct connection));
  if (connection_ptr == NULL)
  {
    LOG_WORKER(LOGLVL_ERROR, worker_no, "memory allocation failed.");
    return false;
  }

  connection_ptr->no = worker_no;
  connection_ptr->upload = upload;
  connection_ptr->host = hostname;
  connection_ptr->state = STATE_NOT_CONNECTED;
  connection_ptr->socket = -1;
  connection_ptr->ip = ip;

  *ctx = connection_ptr;
  *work = worker;
  *cleanup = connection_cleanup;

  return true;
}

//++++++++++++++++++++++++++++++main+++++++++++++++++++++++++++++
int test(int is_ul_test)
{
  uint32_t ip;
  struct worker
  {
  void * ctx;
  work_fn work;
  cleanup_fn cleanup;
  struct pollfd pollfd;
  };
  struct worker * workers = NULL;
  struct pollfd * pollfds = NULL;
  time_ms();
  int ret;
  
  void cleanup(){
   size_t index;
    for (index = 0; index < g_workers; index++)
    {
      if (workers[index].cleanup != NULL)
      {
        workers[index].cleanup(workers[index].ctx);
      }
    }
  }
  
  void free_resource(){
    free(workers);
    free(pollfds);
  }
  
  int nfds, poll_index;
  size_t i;

  workers = calloc(g_workers, sizeof(struct worker));
  if (workers == NULL)
  {
    LERR("memory allocation failed. (workers)");
    ret = 1;
    free_resource();
  }

  pollfds = calloc(g_workers, sizeof(struct pollfd));
  if (pollfds == NULL)
  {
    LERR("memory allocation failed. (pollfds)");
    ret = 1;
    free_resource();
  }

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    LERR("Cannot ignore SIGPIPE. %d (%s)", errno, strerror(errno));
  //  goto fail;
  }

  ip = resolve_host(DEFAULT_HOST);
  if (ip == 0)
  {
    LERR("Cannot catch the host!");
    exit(1);
  }

//++++++++++create 4 workers for each download and upload++++++++
  for (i = 0; i < g_workers; i++)
  {
    if (!create_worker(
          i,
          is_ul_test?"u":"d",
          ip,
          DEFAULT_HOST,
          &workers[i].ctx,
          &workers[i].work,
          &workers[i].cleanup))
    {
      g_workers = 0;
    }
    workers[i].pollfd.fd = -1;
    workers[i].pollfd.revents = 0;
  }
//+++++++++++++++++++++done create workers+++++++++++++++++++++++

  ret = mlockall(MCL_CURRENT | MCL_FUTURE);

  poll_index = 0;
loop:
  assert(poll_index == 0);
//+++++++++++++++++++++++run each worker+++++++++++++++++++++++++ 
  for (i = 0; i < g_workers; i++)
  {
    if (workers[i].work != NULL)
    {
      if (workers[i].pollfd.fd == -1 || /* first time */
          workers[i].pollfd.revents != 0) /* or when there are pending events */
      {
        ret = workers[i].work(
          workers[i].ctx,
          workers[i].pollfd.revents,
          &workers[i].pollfd.fd,
          &workers[i].pollfd.events);
        if (ret < 0)
        {
          ret = -ret;
          cleanup();
        }

        if (ret == 0)
        {
          /* worker done */
          workers[i].work = NULL;
          LOG_WORKER(LOGLVL_INFO, i, "worker done");
          continue;
        }

        workers[i].pollfd.revents = 0;

        assert(workers[i].pollfd.fd != -1);
        assert(workers[i].pollfd.events != 0);
        LOG_WORKER(LOGLVL_DEBUG2, i, "worker waits on %d\n", workers[i].pollfd.fd);
      }
      else
      {
        LOG_WORKER(LOGLVL_DEBUG2, i, "worker still waits on %d\n", workers[i].pollfd.fd);
      }

      pollfds[poll_index].fd = workers[i].pollfd.fd;
      pollfds[poll_index].events = workers[i].pollfd.events;
      pollfds[poll_index].revents = 0;
      poll_index++;
    }
  }//end for()

  if (poll_index == 0)
  {
    ret = 0;
    LINF("no more workers");
    cleanup();
  }

  nfds = poll_index;
  LDBG2("polling %d fds", nfds);
  ret = poll(pollfds, nfds, -1);
  LDBG2("poll() returns %d", ret);
  if (ret == -1)
  {
    LERR("poll() failed. %d (%s)", errno, strerror(errno));
    //goto fail;
  }

  assert(ret > 0);
  poll_index = 0;
  while (ret > 0)
  {
    assert(poll_index < nfds);
    if (pollfds[poll_index].revents != 0)
    {
      for (i = 0; i < g_workers; i++)
      {
        if (workers[i].work != NULL &&
            workers[i].pollfd.fd == pollfds[poll_index].fd)
        {
          workers[i].pollfd.revents = pollfds[poll_index].revents;
          assert(workers[i].pollfd.revents != 0);
          break;
        }
      }
      assert(i < g_workers);        /* fd/worker not found */
      ret--;
    }
    poll_index++;
  }
  poll_index = 0;
  if(1==is_ul_test && 1==is_ul_done)
    return 0;
  if(0==is_ul_test && 1==is_dl_done)
    return 0;

  goto loop;
}

unsigned long time_ms()
{
   struct timeval tv; 
   if(gettimeofday(&tv, NULL) != 0) return 0;
   return (unsigned long)((tv.tv_sec * 1000ul) + (tv.tv_usec / 1000ul));
}
