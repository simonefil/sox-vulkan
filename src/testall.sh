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

check_channel_frequencies() {
	file=$1
	shift
	channel=1

	for expected
	do
		actual=`${bindir}/sox${EXEEXT} "$file" -n remix $channel stat 2>&1 |
			awk '/Rough *frequency/ {print $3}'`
		awk -v actual="$actual" -v expected="$expected" \
			'BEGIN {exit !(actual >= expected - 5 && actual <= expected + 5)}' ||
		{
			echo "Channel $channel of $file is ${actual}Hz, expected ${expected}Hz"
			exit 1
		}
		channel=`expr $channel + 1`
	done
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

if ${bindir}/sox${EXEEXT} --help-format aac 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: aac    Options: -C 128, stereo ADTS"
	${bindir}/sox${EXEEXT} -R -n -r 44100 -c 2 /tmp/aac-stereo.wav \
		synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-stereo.wav -C 128 /tmp/monkey.aac $effect
	${bindir}/sox${EXEEXT} /tmp/monkey.aac /tmp/monkey1.wav $effect

	echo "Format: aac    Options: -C 384, 6-channel ADTS"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 /tmp/aac-5.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 60 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-5.1.wav -C 384 /tmp/aac-5.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-5.1.aac /tmp/aac-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-5.1-decoded.wav`" = 6

	echo "Format: aac    Options: FFmpeg AVOptions passthrough"
	${bindir}/sox${EXEEXT} /tmp/aac-stereo.wav -C 128 \
		--ffmpeg-opts aac_coder=fast:aac_pns=0 /tmp/aac-options.aac
	${bindir}/sox${EXEEXT} --ffmpeg-opts dual_mono_mode=both \
		/tmp/aac-options.aac /tmp/aac-options-decoded.wav

	echo "Format: aac    Options: -C 256, quad PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 4 /tmp/aac-quad.wav \
		synth .1 sine 220 sine 330 sine 440 sine 550 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-quad.wav -C 256 /tmp/aac-quad.aac
	${bindir}/sox${EXEEXT} /tmp/aac-quad.aac /tmp/aac-quad-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-quad-decoded.wav`" = 4 ||
		exit 1
	check_channel_frequencies /tmp/aac-quad-decoded.wav 220 330 440 550

	echo "Format: aac    Options: -C 448, 6.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 7 /tmp/aac-6.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-6.1.wav -C 448 /tmp/aac-6.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-6.1.aac /tmp/aac-6.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-6.1-decoded.wav`" = 7 ||
		exit 1
	check_channel_frequencies /tmp/aac-6.1-decoded.wav \
		220 330 440 110 550 660 770

	echo "Format: aac    Options: -C 512, 7.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 8 /tmp/aac-7.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 sine 880 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav -C 512 /tmp/aac-7.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-7.1.aac /tmp/aac-7.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-7.1-decoded.wav`" = 8 ||
		exit 1
	check_channel_frequencies /tmp/aac-7.1-decoded.wav \
		220 330 440 110 550 660 770 880

	cp /tmp/monkey.aac /tmp/aac-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/aac-autodetect.bin /tmp/aac-autodetect.wav
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
