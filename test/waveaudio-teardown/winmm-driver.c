/* Exercise the actual waveaudio teardown implementation with WinMM stubs. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef void * HANDLE;
typedef HANDLE HWAVEIN;
typedef HANDLE HWAVEOUT;
typedef uint32_t DWORD;
typedef unsigned int MMRESULT;
typedef struct {
  char *lpData;
  DWORD dwBufferLength, dwBytesRecorded, dwUser, dwFlags;
} WAVEHDR, *LPWAVEHDR;
typedef struct { double rate; unsigned channels; } signal_t;
typedef struct { void *priv; signal_t signal; } sox_format_t;

#define num_buffers 4
#define SOX_SUCCESS 0
#define SOX_EOF -1
#define MMSYSERR_NOERROR 0
#define WAVERR_STILLPLAYING 33
#define WHDR_DONE 0x00000001u
#define WHDR_PREPARED 0x00000002u
#define WHDR_INQUEUE 0x00000010u
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffffu

static unsigned long wait_calls, slept, reset_calls, close_calls;
static unsigned long data_freed, events_closed, unprepare_calls, truncated;
static unsigned long reported_failures;
static DWORD ticks;
static MMRESULT close_result, reset_result;
static int stall, fail_unprepare, wait_failure;
static void *test_data;
static WAVEHDR *test_headers;

static void lsx_fail_errno(sox_format_t *ft, int error, const char *fmt, ...)
{
  (void)ft; (void)error; (void)fmt;
  reported_failures++;
}

static DWORD FormatMessageA(DWORD flags, void *src, DWORD code, DWORD lang,
                             char *message, DWORD n, void *args)
{
  (void)flags; (void)src; (void)code; (void)lang;
  (void)message; (void)n; (void)args;
  return 0;
}

static DWORD GetLastError(void) { return 5; }
static DWORD GetTickCount(void) { return ticks; }

static DWORD WaitForSingleObject(HANDLE handle, DWORD timeout)
{
  unsigned i;
  (void)handle;
  wait_calls++;
  if (wait_failure) return WAIT_FAILED;
  if (stall) {
    ticks += timeout;
    return WAIT_TIMEOUT;
  }
  for (i = 0; i < num_buffers; i++) {
    if (test_headers[i].dwFlags & WHDR_INQUEUE) {
      test_headers[i].dwFlags &= ~WHDR_INQUEUE;
      test_headers[i].dwFlags |= WHDR_DONE;
      ticks += 60;
      return WAIT_OBJECT_0;
    }
  }
  return WAIT_TIMEOUT;
}

static unsigned mock_sleep(unsigned seconds)
{
  slept += seconds;
  return 0;
}

static MMRESULT waveOutReset(HWAVEOUT handle)
{
  unsigned i;
  (void)handle;
  reset_calls++;
  for (i = 0; i < num_buffers; i++) {
    if (test_headers[i].dwFlags & WHDR_INQUEUE) {
      truncated++;
      test_headers[i].dwFlags &= ~WHDR_INQUEUE;
      test_headers[i].dwFlags |= WHDR_DONE;
    }
  }
  return reset_result;
}

static MMRESULT waveOutUnprepareHeader(HWAVEOUT handle, LPWAVEHDR hdr, unsigned len)
{
  (void)handle; (void)len;
  unprepare_calls++;
  if (fail_unprepare && unprepare_calls == 1) return WAVERR_STILLPLAYING;
  if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
  hdr->dwFlags &= ~WHDR_PREPARED;
  return MMSYSERR_NOERROR;
}

static MMRESULT waveOutClose(HWAVEOUT handle)
{
  (void)handle;
  close_calls++;
  return close_result;
}

static MMRESULT waveInReset(HWAVEIN handle) { (void)handle; return reset_result; }
static MMRESULT waveInClose(HWAVEIN handle)
{
  (void)handle;
  close_calls++;
  return close_result;
}
static MMRESULT waveInUnprepareHeader(HWAVEIN handle, LPWAVEHDR hdr, unsigned len)
{
  (void)handle; (void)len;
  unprepare_calls++;
  if (fail_unprepare && unprepare_calls == 1) return WAVERR_STILLPLAYING;
  hdr->dwFlags &= ~WHDR_PREPARED;
  return 0;
}

static DWORD CloseHandle(HANDLE event)
{
  (void)event;
  events_closed++;
  return 1;
}

static void tracked_free(void *p)
{
  if (p == test_data) data_freed++;
  free(p);
}

#define sleep mock_sleep
#define free tracked_free
#include "teardown.inc"
#undef sleep
#undef free

static int run_test(const char *name)
{
  sox_format_t ft;
  priv_t *priv;
  int outcome, expected;
  unsigned i;
  unsigned input = strcmp(name, "input-close-failure") == 0;
  unsigned busy = strcmp(name, "idle") != 0 && !input;

  ft.signal.rate = 48000;
  ft.signal.channels = 2;
  priv = calloc(1, sizeof(*priv));
  if (!priv) return 2;
  ft.priv = priv;
  test_headers = priv->headers;
  if (input) priv->hin = (HWAVEIN)(uintptr_t)1;
  else priv->hout = (HWAVEOUT)(uintptr_t)1;
  priv->block_finished_event = (HANDLE)(uintptr_t)2;
  priv->sample_shift = 1;
  priv->data = malloc(1024);
  if (!priv->data) return 2;
  test_data = priv->data;
  for (i = 0; i < num_buffers; i++) {
    priv->headers[i].dwFlags = WHDR_PREPARED;
    priv->headers[i].dwBufferLength = 19200;
  }
  if (busy) for (i = 0; i < 3; i++)
    priv->headers[i].dwFlags |= WHDR_INQUEUE;
  close_result = strstr(name, "close-failure") ? WAVERR_STILLPLAYING : 0;
  stall = strcmp(name, "timeout") == 0;
  wait_failure = strcmp(name, "wait-failure") == 0;
  fail_unprepare = strcmp(name, "unprepare-failure") == 0;
  reset_result = strcmp(name, "reset-failure") == 0 ? 9 : 0;
  ticks = strcmp(name, "tick-wrap") == 0 ? UINT32_MAX - 30u : 0;
  wait_calls = slept = reset_calls = close_calls = 0;
  data_freed = events_closed = unprepare_calls = truncated = 0;
  reported_failures = 0;

  outcome = stop(&ft);
  expected = strstr(name, "failure") || strcmp(name, "timeout") == 0 ?
      SOX_EOF : SOX_SUCCESS;
  if (outcome != expected) goto failed;
  if (slept) goto failed;
  if (truncated && strcmp(name, "timeout") != 0 &&
      strcmp(name, "wait-failure") != 0) goto failed;
  if ((strcmp(name, "drain") == 0 || strcmp(name, "tick-wrap") == 0) &&
      wait_calls < 3) goto failed;
  if (strcmp(name, "idle") == 0 && wait_calls) goto failed;
  if (strcmp(name, "timeout") == 0 && (!reported_failures || !truncated)) goto failed;
  if (strstr(name, "close-failure")) {
    if (ft.priv != NULL || data_freed || events_closed) goto failed;
  } else if (data_freed != 1 || events_closed != 1) goto failed;
  if (reset_calls != (input ? 0ul : 1ul) || close_calls != 1) goto failed;
  if (unprepare_calls != num_buffers) goto failed;
  if (ft.priv) free(ft.priv);
  else { free(priv->data); free(priv); }
  return 0;

failed:
  fprintf(stderr, "%s: result=%d expected=%d sleep=%lu wait=%lu "
          "truncated=%lu data_freed=%lu event_closed=%lu priv=%p "
          "unprepare=%lu failures=%lu\n", name, outcome, expected,
          slept, wait_calls, truncated, data_freed, events_closed,
          ft.priv, unprepare_calls, reported_failures);
  /* The failed baseline may have freed data already; avoid double free. */
  return 2;
}

int main(int argc, char **argv)
{
  if (argc != 2) return 2;
  return run_test(argv[1]);
}
