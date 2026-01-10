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

if ${bindir}/sox${EXEEXT} --help-format ac3 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: ac3    Options: -C 448, 6-channel"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 /tmp/ac3-5.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 60 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} /tmp/ac3-5.1.wav -C 448 /tmp/ac3-5.1.ac3
	${bindir}/sox${EXEEXT} /tmp/ac3-5.1.ac3 /tmp/ac3-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/ac3-5.1-decoded.wav`" = 6

	echo "Format: ac3    Options: FFmpeg AVOptions passthrough"
	${bindir}/sox${EXEEXT} /tmp/ac3-5.1.wav -C 448 \
		--ffmpeg-opts dialnorm=-27:center_mixlev=0.707:surround_mixlev=0.5 \
		/tmp/ac3-options.ac3
	${bindir}/sox${EXEEXT} /tmp/ac3-options.ac3 /tmp/ac3-options-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/ac3-options-decoded.wav`" = 6

	cp /tmp/ac3-5.1.ac3 /tmp/ac3-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/ac3-autodetect.bin \
		/tmp/ac3-autodetect.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/ac3-autodetect.wav`" = 6
fi

if ${bindir}/sox${EXEEXT} --help-format eac3 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: eac3   Options: -C 768, 6-channel"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 /tmp/eac3-5.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 60 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav -C 768 /tmp/eac3-5.1.eac3
	${bindir}/sox${EXEEXT} /tmp/eac3-5.1.eac3 /tmp/eac3-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/eac3-5.1-decoded.wav`" = 6

	echo "Format: eac3   Options: FFmpeg AVOptions passthrough"
	${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav -C 768 \
		--ffmpeg-opts dialnorm=-27:dmix_mode=loro:stereo_rematrixing=0 \
		/tmp/eac3-options.eac3
	${bindir}/sox${EXEEXT} --ffmpeg-opts drc_scale=0.5 \
		/tmp/eac3-options.eac3 /tmp/eac3-options-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/eac3-options-decoded.wav`" = 6

	if ${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav \
		--ffmpeg-opts unknown_sox_test_option=1 \
		/tmp/eac3-unknown-option.eac3 2>/dev/null; then
		echo "Unknown FFmpeg option was not rejected"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav \
		--ffmpeg-opts b=192000 /tmp/eac3-reserved-option.eac3 \
		2>/dev/null; then
		echo "SoX-controlled FFmpeg option was not rejected"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav \
		--ffmpeg-opts dialnorm=-27 /tmp/eac3-options.wav \
		2>/dev/null; then
		echo "FFmpeg options were accepted by a non-FFmpeg format"
		exit 1
	fi

	cp /tmp/eac3-5.1.eac3 /tmp/eac3-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/eac3-autodetect.bin \
		/tmp/eac3-autodetect.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/eac3-autodetect.wav`" = 6

	cp /tmp/eac3-5.1.eac3 /tmp/eac3-mislabeled.ac3
	cp /tmp/ac3-5.1.ac3 /tmp/ac3-mislabeled.eac3
	test "`${bindir}/sox${EXEEXT} --i -e /tmp/eac3-mislabeled.ac3`" = \
		"ATSC A/52 E-AC-3"
	test "`${bindir}/sox${EXEEXT} --i -e /tmp/ac3-mislabeled.eac3`" = \
		"ATSC A/52 AC-3"
fi
