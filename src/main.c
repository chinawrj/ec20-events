#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifndef CURL_PATH
#define CURL_PATH "/usr/bin/curl"
#endif
#include "sms.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static const char *root = "/data/ec20-events";
static volatile sig_atomic_t stopping;
static void stop(int sig) {
  (void)sig;
  stopping = 1;
}
static long mono(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    exit(2);
  return t.tv_sec;
}
static void path(char *b, const char *s) {
  if (snprintf(b, PATH_MAX, "%s/%s", root, s) >= PATH_MAX)
    exit(2);
}
static int writeall(int f, const char *b, size_t n) {
  while (n) {
    ssize_t w = write(f, b, n);
    if (w <= 0) {
      if (w < 0 && errno == EINTR)
        continue;
      return -1;
    }
    b += w;
    n -= (size_t)w;
  }
  return 0;
}
static int save(const char *p, const char *s) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", p, (long)getpid()) >= PATH_MAX)
    return -1;
  int f =
      open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (f < 0)
    return -1;
  int r = writeall(f, s, strlen(s));
  if (!r)
    r = fsync(f);
  if (close(f))
    r = -1;
  if (!r)
    r = rename(tmp, p);
  if (r)
    unlink(tmp);
  return r;
}
static int load(const char *p, char *s, size_t cap) {
  int f = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (f < 0)
    return -1;
  size_t n = 0;
  while (n < cap - 1) {
    ssize_t r = read(f, s + n, cap - 1 - n);
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0) {
      close(f);
      return -1;
    }
    if (!r)
      break;
    n += (size_t)r;
  }
  char extra;
  ssize_t r = read(f, &extra, 1);
  close(f);
  s[n] = 0;
  return r == 0 ? (int)n : -1;
}
static void syncdir(const char *p) {
  int f = open(p, O_RDONLY | O_CLOEXEC);
  if (f >= 0) {
    fsync(f);
    close(f);
  }
}
static int lockfile(const char *name, int wait) {
  char p[PATH_MAX];
  path(p, name);
  int f = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (f < 0)
    return -1;
  if (flock(f, LOCK_EX | (wait ? 0 : LOCK_NB))) {
    close(f);
    return -1;
  }
  return f;
}
static void unlock(int f) {
  if (f >= 0) {
    flock(f, LOCK_UN);
    close(f);
  }
}
static int setup(void) {
  umask(077);
  if (mkdir(root, 0700) && errno != EEXIST)
    return -1;
  struct stat st;
  if (lstat(root, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 077))
    return -1;
  const char *dirs[] = {"outbox", "state", "pending"};
  for (size_t i = 0; i < 3; i++) {
    char p[PATH_MAX];
    path(p, dirs[i]);
    if (mkdir(p, 0700) && errno != EEXIST)
      return -1;
  }
  return 0;
}
static int countqueue(void) {
  char p[PATH_MAX];
  path(p, "outbox");
  DIR *d = opendir(p);
  if (!d)
    return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (!strncmp(e->d_name, "event.", 6))
      n++;
  closedir(d);
  return n;
}
static int enqueue(const char *title, const char *body) {
  if (!*body || strlen(body) > 1024 || strlen(title) > 150)
    return -1;
  int lk = lockfile("queue.lock", 1);
  if (lk < 0)
    return -1;
  int count = countqueue();
  if (count < 0 || count >= 100) {
    unlock(lk);
    return -1;
  }
  char stage[PATH_MAX], final[PATH_MAX], t[PATH_MAX], m[PATH_MAX],
      dir[PATH_MAX];
  path(stage, "outbox/.stage.XXXXXX");
  if (!mkdtemp(stage)) {
    unlock(lk);
    return -1;
  }
  snprintf(t, sizeof t, "%s/title", stage);
  snprintf(m, sizeof m, "%s/message", stage);
  int r = save(t, title) || save(m, body);
  syncdir(stage);
  if (!r) {
    char part[128];
    snprintf(part, sizeof part, "outbox/event.%s", strrchr(stage, '.') + 1);
    path(final, part);
    r = rename(stage, final);
  }
  if (r) {
    unlink(t);
    unlink(m);
    rmdir(stage);
  }
  path(dir, "outbox");
  syncdir(dir);
  unlock(lk);
  return r ? -1 : 0;
}
static void state(const char *s) {
  char p[PATH_MAX], old[256];
  path(p, "state/sender-status");
  if (load(p, old, sizeof old) >= 0 && !strcmp(old, s))
    return;
  (void)save(p, s);
}
static int keyvalid(const char *name) {
  char p[PATH_MAX], s[64];
  path(p, name);
  struct stat st;
  if (lstat(p, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
      st.st_uid != geteuid())
    return 0;
  if (load(p, s, sizeof s) != 30)
    return 0;
  for (int i = 0; i < 30; i++)
    if (!isalnum((unsigned char)s[i]))
      return 0;
  return 1;
}
/* Success accepts only an exact integer status=1 top-level field. API's other
 * fields may contain strings. */
static int accepted(const char *s) {
  int depth = 0;
  while (*s) {
    if (*s == '{') {
      depth++;
      s++;
      continue;
    }
    if (*s == '}') {
      depth--;
      s++;
      continue;
    }
    if (*s == '"') {
      s++;
      const char *start = s;
      while (*s && *s != '"') {
        if (*s == '\\' && s[1])
          s++;
        s++;
      }
      size_t n = (size_t)(s - start);
      if (!*s)
        return 0;
      s++;
      if (depth == 1 && n == 6 && !memcmp(start, "status", 6)) {
        while (isspace((unsigned char)*s))
          s++;
        if (*s++ != ':')
          return 0;
        while (isspace((unsigned char)*s))
          s++;
        if (*s++ != '1')
          return 0;
        while (isspace((unsigned char)*s))
          s++;
        return *s == ',' || *s == '}';
      }
    } else
      s++;
  }
  return 0;
}
static int flush(void) {
  int lk = lockfile("sender.lock", 0);
  if (lk < 0)
    return 4;
  if (!keyvalid("app-token") || !keyvalid("user-key")) {
    state("credentials-missing-or-insecure\n");
    unlock(lk);
    return 5;
  }
  if (time(NULL) < 1735689600) {
    state("clock-invalid\n");
    unlock(lk);
    return 6;
  }
  char dir[PATH_MAX];
  path(dir, "outbox");
  struct dirent **items = 0;
  int count = scandir(dir, &items, 0, alphasort), result = 0;
  if (count < 0) {
    unlock(lk);
    return 7;
  }
  for (int i = 0; i < count; i++) {
    if (result || stopping || strncmp(items[i]->d_name, "event.", 6)) {
      free(items[i]);
      continue;
    }
    char item[PATH_MAX], title[PATH_MAX], msg[PATH_MAX], token[PATH_MAX + 16],
        user[PATH_MAX + 16], ta[PATH_MAX + 16], ma[PATH_MAX + 16], p[PATH_MAX],
        response[PATH_MAX], reply[8192];
    snprintf(item, sizeof item, "%s/%s", dir, items[i]->d_name);
    snprintf(title, sizeof title, "%s/title", item);
    snprintf(msg, sizeof msg, "%s/message", item);
    path(p, "app-token");
    snprintf(token, sizeof token, "token@%s", p);
    path(p, "user-key");
    snprintf(user, sizeof user, "user@%s", p);
    snprintf(ta, sizeof ta, "title@%s", title);
    snprintf(ma, sizeof ma, "message@%s", msg);
    path(response, "state/response.tmp");
    pid_t child = fork();
    if (child == 0) {
      int null = open("/dev/null", O_RDWR);
      if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        close(null);
      }
      /* -q disables ~/.curlrc. --fail rejects HTTP errors; never follow
       * redirects or disable verification. */
      execl(CURL_PATH, "curl", "-q", "--silent", "--fail", "--proto", "=https",
            "--connect-timeout", "10", "--max-time", "30", "--max-filesize",
            "8192", "--output", response, "--data-urlencode", token,
            "--data-urlencode", user, "--data-urlencode", ta,
            "--data-urlencode", ma, "--data-urlencode", "priority=0",
            "https://api.pushover.net/1/messages.json", (char *)0);
      _exit(127);
    }
    int status = 0;
    pid_t w = -1;
    if (child > 0)
      do {
        w = waitpid(child, &status, 0);
      } while (w < 0 && errno == EINTR);
    if (w == child && child > 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && load(response, reply, sizeof reply) > 0 &&
        accepted(reply)) {
      int q = lockfile("queue.lock", 1);
      if (q >= 0) {
        char acked[PATH_MAX], at[PATH_MAX], am[PATH_MAX];
        snprintf(acked, sizeof acked, "%s/.acked.%s", dir,
                 items[i]->d_name + 6);
        if (rename(item, acked))
          result = 7;
        else {
          syncdir(dir); /* Atomic acknowledgement before deleting component
                           files. */
          snprintf(at, sizeof at, "%s/title", acked);
          snprintf(am, sizeof am, "%s/message", acked);
          unlink(at);
          unlink(am);
          rmdir(acked);
          syncdir(dir);
        }
        unlock(q);
      } else
        result = 7;
      state("accepted-by-pushover\n");
    } else {
      state("https-or-api-failed-event-retained\n");
      result = 7;
    }
    unlink(response);
    free(items[i]);
    if (!result && !stopping)
      sleep(1);
  }
  free(items);
  unlock(lk);
  return result;
}
static int fd, busy, sms_index = -1, header, discard, cycle, initialized;
static long deadline, next_poll, next_sms;
static char line[2048], pdu[1025], last[3][128], pending_file[PATH_MAX];
static size_t used;
static int read_store_sm, pdu_mode;
static void command(const char *s, int kind) {
  if (writeall(fd, s, strlen(s)))
    stopping = 1;
  busy = kind;
  deadline = mono() + 8;
}
static void notice(const char *s) {
  char store[8];
  int index = -1;
  if (sscanf(s, "+CMTI: \"%7[^\"]\",%d", store, &index) != 2 || index < 0 ||
      index > 255)
    return;
  if (strcmp(store, "SM")) {
    enqueue(
        "EC20 SMS notice",
        "A new SMS arrived outside SM storage; use the modem UI to read it.");
    return;
  }
  char p[PATH_MAX], part[64];
  snprintf(part, sizeof part, "pending/%d", index);
  path(p, part);
  if (save(p, "pending\n"))
    state("sms-persist-failed\n");
  else {
    path(p, "pending");
    syncdir(p);
  }
}
static int pending(void) {
  char p[PATH_MAX];
  path(p, "pending");
  DIR *d = opendir(p);
  if (!d)
    return -1;
  struct dirent *e;
  int index = -1;
  while ((e = readdir(d))) {
    if (!*e->d_name || strspn(e->d_name, "0123456789") != strlen(e->d_name))
      continue;
    long n = strtol(e->d_name, 0, 10);
    if (n < 0 || n > 255)
      continue;
    index = (int)n;
    snprintf(pending_file, sizeof pending_file, "%s/pending/%d", root, index);
    break;
  }
  closedir(d);
  return index;
}
static void receive_sms(void) {
  char seen[PATH_MAX], part[64], old[1025], body[1025];
  snprintf(part, sizeof part, "state/sms-%d.pdu", sms_index);
  path(seen, part);
  if (load(seen, old, sizeof old) >= 0 && !strcmp(old, pdu)) {
    unlink(pending_file);
    return;
  }
  if (sms_decode(pdu, body,
                 sizeof body)) { /* Retain malformed PDU for local diagnosis, do
                                    not leak it into logs. */
    snprintf(body, sizeof body,
             "A new SMS could not be decoded (SIM index %d); inspect it in the "
             "modem UI.",
             sms_index);
  }
  if (!enqueue("EC20 SMS", body) && !save(seen, pdu))
    unlink(pending_file);
}
static void handle(const char *s) {
  if (!strncmp(s, "+CMTI:", 6)) {
    notice(s);
    return;
  }
  const char *prefix[] = {"+CPIN:", "+CEREG:", "+CGATT:"};
  for (int i = 0; i < 3; i++)
    if (!strncmp(s, prefix[i], strlen(prefix[i]))) {
      const char *v = s + strlen(prefix[i]);
      while (*v == ' ')
        v++;
      if (strlen(v) >= sizeof last[i])
        return;
      /* Only polling replies are compared: unsolicited CEREG has a different
       * grammar. */
      if (busy != i + 1)
        return;
      if (last[i][0] && strcmp(last[i], v)) {
        char body[300];
        snprintf(body, sizeof body, "%s %s -> %s", prefix[i], last[i], v);
        if (enqueue("EC20 state change", body))
          return;
      }
      snprintf(last[i], sizeof last[i], "%s", v);
    }
  if (busy == 7 && !strncmp(s, "+CMGF:", 6)) {
    int m = -1;
    sscanf(s + 6, "%d", &m);
    pdu_mode = m == 0;
  }
  if (busy == 8 && !strncmp(s, "+CPMS:", 6)) {
    char st[8] = {0};
    sscanf(s, "+CPMS: \"%7[^\"]\"", st);
    read_store_sm = !strcmp(st, "SM");
  }
  if (busy == 9 && !strncmp(s, "+CMGR:", 6))
    header = 1;
  else if (busy == 9 && header && *s && strlen(s) <= 1024 &&
           strspn(s, "0123456789abcdefABCDEF") == strlen(s)) {
    snprintf(pdu, sizeof pdu, "%s", s);
    header = 0;
  }
  if (!strcmp(s, "OK") || !strcmp(s, "ERROR") ||
      !strncmp(s, "+CME ERROR:", 11) || !strncmp(s, "+CMS ERROR:", 11)) {
    int kind = busy, ok = !strcmp(s, "OK");
    busy = 0;
    header = 0;
    if (kind == 9) {
      if (ok && *pdu)
        receive_sms();
      next_sms = mono() + 30;
      next_poll = mono() + 1;
    } else if (kind == 7) {
      if (ok && pdu_mode)
        command("AT+CPMS?\r", 8);
      else
        next_sms = mono() + 30;
    } else if (kind == 8) {
      if (ok && read_store_sm) {
        char c[64];
        snprintf(c, sizeof c, "AT+CMGR=%d\r", sms_index);
        *pdu = 0;
        command(c, 9);
      } else
        next_sms = mono() + 30;
    } else if (kind == 4) {
      if (!ok)
        state("CNMI-setup-failed\n");
      initialized = 1;
      next_poll = mono() + 1;
    } else
      next_poll = mono() + 10;
  }
}
static int monitor(const char *port) {
  int lk = lockfile("collector.lock", 0);
  if (lk < 0)
    return 4;
  fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    unlock(lk);
    return 4;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  struct termios original, t;
  int restore = !tcgetattr(fd, &original);
  if (restore) {
    t = original;
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd, TCSANOW, &t);
  }
  char boot[128], p[PATH_MAX], old[128];
  path(p, "state/boot-id");
  if (load("/proc/sys/kernel/random/boot_id", boot, sizeof boot) > 0 &&
      (load(p, old, sizeof old) < 0 || strcmp(boot, old))) {
    if (!enqueue("EC20 started",
                 "Native event service started on the modem. SMS arrival and "
                 "SIM/network changes are monitored."))
      save(p, boot);
  }
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  signal(SIGHUP, stop);
  signal(SIGPIPE, SIG_IGN);
  next_poll = mono();
  next_sms = mono();
  long next_flush = mono() + 5;
  unsigned backoff = 60;
  pid_t sender = 0;
  while (!stopping) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = {1, 0};
    int ready = select(fd + 1, &r, 0, 0, &tv);
    if (ready < 0 && errno != EINTR)
      break;
    if (ready > 0) {
      char b[512];
      ssize_t n = read(fd, b, sizeof b);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
          continue;
        break;
      }
      for (ssize_t i = 0; i < n; i++) {
        if (b[i] == '\r' || b[i] == '\n') {
          if (used && !discard) {
            line[used] = 0;
            handle(line);
          }
          used = 0;
          discard = 0;
        } else if (!discard) {
          if (used < sizeof line - 1)
            line[used++] = b[i];
          else {
            used = 0;
            discard = 1;
          }
        }
      }
    }
    long now = mono();
    if (busy && now >= deadline) {
      busy = 0;
      header = 0;
      *pdu = 0;
      next_poll = now + 10;
      next_sms = now + 30;
      state("AT-timeout\n");
    }
    if (!busy) {
      if (!initialized &&
          now >= next_poll) { /* SM arrivals are indications, not direct +CMT
                                 payloads. No historical SMS scan. */
        command("AT+CNMI=2,1,0,0,0\r", 4);
        next_poll = now + 30;
      } else if (initialized && now >= next_sms &&
                 (sms_index = pending()) >= 0) {
        pdu_mode = 0;
        read_store_sm = 0;
        command("AT+CMGF?\r", 7);
        next_sms = now + 30;
      } else if (initialized && now >= next_poll) {
        const char *cmd[] = {"AT+CPIN?\r", "AT+CEREG?\r", "AT+CGATT?\r"};
        command(cmd[cycle], cycle + 1);
        cycle = (cycle + 1) % 3;
        next_poll = now + 10;
      }
    }
    if (sender > 0) {
      int status;
      pid_t w = waitpid(sender, &status, WNOHANG);
      if (w == sender) {
        sender = 0;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
          backoff = 60;
        else if (backoff < 3600)
          backoff = backoff > 1800 ? 3600 : backoff * 2;
        next_flush = now + backoff;
      }
    }
    if (!sender && now >= next_flush) {
      if (countqueue() > 0) {
        sender = fork();
        if (!sender) {
          close(fd);
          close(lk);
          _exit(flush());
        }
      }
      next_flush = now + 60;
    }
  }
  if (sender >
      0) { /* Finish the current curl transaction, then stop the batch. */
    kill(sender, SIGTERM);
    while (waitpid(sender, 0, 0) < 0 && errno == EINTR) {
    }
  }
  if (restore)
    tcsetattr(fd, TCSANOW, &original);
  close(fd);
  unlock(lk);
  return stopping ? 0 : 4;
}
static int supervise(const char *port) {
  int lk = lockfile("supervisor.lock", 0);
  if (lk < 0)
    return 4;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = stop;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGINT, &sa, 0);
  sigaction(SIGHUP, &sa, 0);
  while (!stopping) {
    pid_t p = fork();
    if (!p) {
      close(lk);
      _exit(monitor(port));
    }
    if (p < 0)
      break;
    int status;
    if (stopping)
      kill(p, SIGTERM);
    while (waitpid(p, &status, 0) < 0) {
      if (errno != EINTR)
        break;
      if (stopping)
        kill(p, SIGTERM);
    }
    if (stopping)
      break;
    state("collector-restarting\n");
    for (int i = 0; i < 30 && !stopping; i++)
      sleep(1);
  }
  unlock(lk);
  return 0;
}
int main(int argc, char **argv) {
  const char *env = getenv("EC20_EVENTS_DIR");
  if (env && *env)
    root = env;
  if (argc == 2 && !strcmp(argv[1], "version")) {
    puts("ec20-events 0.1.0 (native C)");
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "decode")) {
    char body[1025];
    if (sms_decode(argv[2], body, sizeof body))
      return 2;
    puts(body);
    return 0;
  }
  if (argc < 2) {
    fprintf(stderr,
            "Usage: ec20-events version | monitor [port] | supervise [port] | "
            "enqueue title body | flush | status | decode HEX\n");
    return 2;
  }
  if (setup()) {
    fprintf(stderr, "Event directory must be owned by this user, mode 0700\n");
    return 2;
  }
  if (!strcmp(argv[1], "enqueue") && argc == 4)
    return enqueue(argv[2], argv[3]) ? 3 : 0;
  if (!strcmp(argv[1], "flush") && argc == 2)
    return flush();
  if (!strcmp(argv[1], "status") && argc == 2) {
    char p[PATH_MAX], s[256];
    int lk = lockfile("collector.lock", 0);
    printf("collector=%s\nqueued=%d\ncredentials=%s\n",
           lk < 0 ? "running-or-locked" : "stopped", countqueue(),
           keyvalid("app-token") && keyvalid("user-key")
               ? "configured"
               : "missing-or-insecure");
    unlock(lk);
    path(p, "state/sender-status");
    if (load(p, s, sizeof s) > 0)
      printf("last_status=%s", s);
    return 0;
  }
  if ((!strcmp(argv[1], "monitor") || !strcmp(argv[1], "supervise")) &&
      argc <= 3)
    return !strcmp(argv[1], "monitor")
               ? monitor(argc == 3 ? argv[2] : "/dev/smd8")
               : supervise(argc == 3 ? argv[2] : "/dev/smd8");
  return 2;
}
