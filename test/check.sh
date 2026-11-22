#! /bin/sh

# Run all the tests
#
# The suite comes from sox_ng (https://codeberg.org/sox_ng/sox_ng, test/).
#
# To test a different executable than the "sox" in PATH,
# set environment variable "sox" to the name or path to the executable.
# e.g.
#	sox=$HOME/sox-14.4.2/src/sox sh check.sh
# or, to test a CMake build of this tree
#	sox=build/src/sox sh test/check.sh
# A relative path is taken from the current directory, and check.sh can be
# run from anywhere: it moves into its own directory before running the tests.
#
# Environment variable "samples" names the helper built from samples.c,
# which the CMake build provides; without it, bit-depth compiles its own.
#
# You can also just run one or more tests,
# e.g.
#	sh check.sh CVE-2017-18189

usage() {
    {
	echo "Usage: check.sh [-n] [test] ..."
	echo "-n  Don't print the legend"
    } 1>&2
}

# Process command line arguments

legend=true
while [ $# -gt 0 ]
do
    case "$1" in
    -n) legend=false; shift;;
    -*) usage; exit 1;;
    *)  break;;
    esac
done

args="$*"

# By default, check the sox in PATH
sox=${sox:-sox}
case "$sox" in
/*) ;;
*/*) sox="`pwd`/$sox" ;;
esac
export sox

cd "`dirname "$0"`" || exit 1

if [ -z "$args" ]
then
    LC_COLLATE=C       # Do upper case before lower case
    export LC_COLLATE
    args="`LC_COLLATE=C ls`"
fi

# We don't want memory leaks to report a failure.
ASAN_OPTIONS=detect_leaks=0
export ASAN_OPTIONS

errs=/tmp/check$$

# Print the legend
$legend && cat << \EOF
Legend
OK   The test succeeded and sox succeeded or failed as it should
FAIL The test ran but gave the wrong result.
SUCC sox "succeeded" (exit 0) when it should have failed (exit 2)
ABRT sox Aborted (core dumped)
SEGV sox got a Segmentation fault (core dumped)
FPE  sox got a Floating Point Exception (core dumped)
ILL  sox got a Illegal instruction exception (core dumped)
LOOP sox ran for more than four minutes.
EXEC Can't execute the sox binary. Missing shared libraries also provoke this.
VOID This test cannot be run with the sox you have installed
ASAN The Address Sanitizer reports problems other than memory leaks:
     buffer overflows, freeing free memory, allocating until running out of VM.
     It means exit(1), which sox only gives for command-line syntax errors.
     Other failures (SEGV, FPE etc) will be caught by ASAN in such builds.
     You will also get an ASAN error if your sox wasn't built with support
     for the input or output format, in which case it exits 1.

EOF

# The exit code the whole script should give: 0 unless anything fails
endexit=0

for a in $args
do
    if [ -d "$a" -a -x "$a"/run ]
    then
	printf "${a}: "
        (
	    cd "$a"
	    if [ ! -f run ]
	    then
		echo "No test file"
		continue
	    fi
	    # cfarm185 takes 3m09 to run CVE-2019-8357 so max 4m.
	    # ulimit -t does not stop a process on macOS, so with perl the
	    # test runs in its own process group, killed as a whole after
	    # four minutes of wall time with the same status as ulimit's kill.
	    if command -v perl > /dev/null 2>&1
	    then
		perl -e '
		    my $limit = shift;
		    my $pid = fork;
		    die "fork: $!" unless defined $pid;
		    if ($pid == 0) { setpgrp(0, 0); exec @ARGV or exit 127; }
		    $SIG{ALRM} = sub { kill "KILL", -$pid; };
		    alarm $limit;
		    waitpid($pid, 0);
		    exit(($? & 127) ? 128 + ($? & 127) : $? >> 8);
		' 240 sh run
	    else
		ulimit -t 240
		sh run
	    fi
	) > $errs 2>&1
	status=$?
	case $status in
	0)   result=OK ;;
	1)   result=ASAN ;;
	2)   result=FAIL ;;
	127) result=EXEC ;;
	132) result=ILL  ;;
	134) result=ABRT ;;
	136) result=FPE  ;;
	137) result=LOOP ;;
	139) result=SEGV ;;
	254) result=VOID ;;
	255) result=SUCC ;;
	*)   result=$status ;;
	esac
	echo $result
	if [ $status -ne 0 ] && [ $status -ne 254 ]
	then
	    cat $errs
	    endexit=$status
	fi
	rm $errs
    fi
done

exit $endexit
