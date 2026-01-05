bindir="."
srcdir="."
effect=""

if [ -f ./sox.exe ] ; then
  EXEEXT=".exe"
else
  EXEEXXT=""
fi

# Allow user to override paths.  Useful for testing an installed
# sox.
while [ $# -ne 0 ]; do
    case "$1" in
        --bindir=*)
        bindir=`echo $1 | sed 's/.*=//'`
        ;;

        -i)
        shift
        bindir=$1
        ;;

        --srcdir=*)
        srcdir=`echo $1 | sed 's/.*=//'`
        ;;

        -c)
        shift
        srcdir=$1
        ;;

        *)
        effect="$effect $1"
    esac
    shift
done

t() {
	format=$1
	shift
	opts="$*"

	echo "Format: $format   Options: $opts"
	${bindir}/sox${EXEEXT} ${srcdir}/monkey.wav $opts /tmp/monkey.$format $effect
	${bindir}/sox${EXEEXT} $opts /tmp/monkey.$format /tmp/monkey1.wav  $effect
}
t 8svx
t aiff
t aifc
t au 
t avr -e unsigned-integer
t cdr
t cvs
t dat
t hcom -r 22050
t maud
t prc
t prc -e signed-integer
t sf 
t smp
t sndt 
t txw
t ub -r 8130
t vms
t voc
t vox -r 8130
t wav
t wve

if ${bindir}/sox${EXEEXT} --help-format opus 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: opus   Options: -C 96"
	${bindir}/sox${EXEEXT} ${srcdir}/monkey.wav -C 96 /tmp/monkey.opus $effect
	${bindir}/sox${EXEEXT} /tmp/monkey.opus /tmp/monkey1.wav $effect

	echo "Format: opus   Options: 6-channel mapping family 1"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 /tmp/opus-5.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 60 sine 550 sine 660
	${bindir}/sox${EXEEXT} /tmp/opus-5.1.wav -C 384 /tmp/opus-5.1.opus
	${bindir}/sox${EXEEXT} /tmp/opus-5.1.opus /tmp/opus-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/opus-5.1-decoded.wav`" = 6
fi
