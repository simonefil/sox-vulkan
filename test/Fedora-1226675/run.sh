#! /bin/sh

# https://bugzilla.redhat.com/show_bug.cgi?id=1226675
# [abrt] sox: startwrite(): sox killed by SIGFPE
#
# Reported by Sylvain Petreolle 2015-05-31
# play -q /home/syl/freedos/vgacpy6/vgacpy3.voc
# Truncated backtrace:
# Thread no. 1 (4 frames)
#  #0 startwrite at ao.c:39
#  #1 try_device at sox.c:2547
#  #2 set_default_device at sox.c:2569
#  #3 parse_options_and_filenames at sox.c:2681
#
# It seems that ao.c:38
# ao->buf_size = sox_globals.bufsiz - (sox_globals.bufsiz % (ft->encoding.bits_per_sample >> 3));
# divides by zero in your case but I can not reproduce the crash.

# 14.4.2
# Plays the file and exits 0, also with Address Sanitizer.

# Debian bookworm i386
# Plays the file and exits 0.

# 42b355
# Plays the file and exits 0.

# Fedora 40 x86_64
# Plays the file and exits 0 but says:
# sox FAIL sox: `vgacpy3.voc' premature EOF: Numerical result out of range

# Fedora 40 x86_64 under "mock"
# First it says "No default audio device configured", so I
# export AUDIODRIVER=ao				# for SoX
# echo "default_driver=null" > ~/.libao		# for libao
# and it succeeds without the "Numerical result" message.

rm -f core

${sox:-sox} vgacpy3.voc -d
status=$?

rm -f core out.wav

exit $status
