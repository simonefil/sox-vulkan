set -e

bindir="."
srcdir="."
effect=""

if [ -f ./sox.exe ] ; then
  EXEEXT=".exe"
else
  EXEEXT=""
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

test_explicit_layout() {
	format=$1
	source=$2
	layout=$3
	id=$4
	channels=$5
	compression=$6
	output=/tmp/${format}-${id}.${format}
	decoded=/tmp/${format}-${id}-decoded.wav
	log=/tmp/${format}-${id}-decode.log

	echo "Format: $format   Options: --channel-layout $layout"
	${bindir}/sox${EXEEXT} --help-format "$format" 2>/dev/null |
		grep -F "$layout" >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} "$source" -C "$compression" --channel-layout "$layout" "$output"
	${bindir}/sox${EXEEXT} "$output" "$decoded" 2>"$log"
	test "`${bindir}/sox${EXEEXT} --i -c "$decoded"`" = "$channels" ||
		exit 1
}

test_alac_layout() {
	source=$1
	layout=$2
	id=$3
	channels=$4
	pcm=/tmp/alac-layout-${id}.wav
	reference=/tmp/alac-layout-${id}-reference.s16
	output=/tmp/alac-layout-${id}.m4a
	decoded=/tmp/alac-layout-${id}-decoded.s16

	echo "Format: m4a    Options: --channel-layout $layout"
	${bindir}/sox${EXEEXT} --help-format m4a 2>/dev/null |
		grep -F "$layout" >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} "$source" -c "$channels" -b 16 "$pcm"
	${bindir}/sox${EXEEXT} "$pcm" -t s16 "$reference"
	${bindir}/sox${EXEEXT} "$pcm" --channel-layout "$layout" "$output"
	${bindir}/sox${EXEEXT} "$output" -t s16 "$decoded"
	cmp "$reference" "$decoded" ||
		exit 1
}

check_layout_alias() {
	canonical=$1
	alias=$2

	if ${bindir}/sox${EXEEXT} --help-format "$canonical" 2>/dev/null |
		grep -q '^Output channel layouts'; then
		${bindir}/sox${EXEEXT} --help-format "$alias" 2>/dev/null |
			grep -q '^Output channel layouts' ||
			exit 1
	fi
}

check_layout_alias aac adts
check_layout_alias latm loas
check_layout_alias eac3 ec3
check_layout_alias truehd thd

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

if ${bindir}/sox${EXEEXT} --help-format m4a 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: m4a    Options: -C 2, 16-bit stereo ALAC"
	${bindir}/sox${EXEEXT} -R -n -r 44100 -c 2 -b 16 /tmp/alac-stereo.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/alac-stereo.wav -t s16 /tmp/alac-stereo-reference.s16
	${bindir}/sox${EXEEXT} /tmp/alac-stereo.wav -C 2 /tmp/alac-stereo.m4a
	${bindir}/sox${EXEEXT} /tmp/alac-stereo.m4a -t s16 /tmp/alac-stereo-decoded.s16
	cmp /tmp/alac-stereo-reference.s16 /tmp/alac-stereo-decoded.s16 ||
		exit 1

	echo "Format: m4a    Options: -C 0, 24-bit stereo ALAC"
	${bindir}/sox${EXEEXT} -R -n -r 96000 -c 2 -b 24 /tmp/alac-24.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/alac-24.wav -t s32 /tmp/alac-24-reference.s32
	${bindir}/sox${EXEEXT} /tmp/alac-24.wav -C 0 /tmp/alac-24.m4a
	${bindir}/sox${EXEEXT} /tmp/alac-24.m4a -t s32 /tmp/alac-24-decoded.s32
	cmp /tmp/alac-24-reference.s32 /tmp/alac-24-decoded.s32 ||
		exit 1

	echo "Format: m4a    Options: lossless 5.1(back) ALAC"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 -b 16 \
		/tmp/alac-5.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 110 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} /tmp/alac-5.1.wav -t s16 /tmp/alac-5.1-reference.s16
	${bindir}/sox${EXEEXT} /tmp/alac-5.1.wav /tmp/alac-5.1.m4a
	${bindir}/sox${EXEEXT} /tmp/alac-5.1.m4a -t s16 /tmp/alac-5.1-decoded.s16
	cmp /tmp/alac-5.1-reference.s16 /tmp/alac-5.1-decoded.s16 ||
		exit 1

	cp /tmp/alac-stereo.m4a /tmp/alac-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/alac-autodetect.bin /tmp/alac-autodetect.wav

	echo "Format: m4a    Options: MPEG 4.0 B without automatic remix"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 4 -b 16 /tmp/alac-quad.wav synth .1 sine 220 sine 330 sine 440 sine 550
	${bindir}/sox${EXEEXT} /tmp/alac-quad.wav -t s16 /tmp/alac-quad-reference.s16
	${bindir}/sox${EXEEXT} /tmp/alac-quad.wav /tmp/alac-quad.m4a 2>/tmp/alac-quad-encode.log
	grep -q 'MPEG 4.0 B without remixing' /tmp/alac-quad-encode.log ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/alac-quad.m4a -t s16 /tmp/alac-quad-decoded.s16
	cmp /tmp/alac-quad-reference.s16 /tmp/alac-quad-decoded.s16 ||
		exit 1

	echo "Format: m4a    Options: Apple AAC 6.1 without automatic remix"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 7 -b 16 \
		/tmp/alac-6.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 110 sine 550 sine 660 sine 770 vol .1
	${bindir}/sox${EXEEXT} /tmp/alac-6.1.wav -t s16 /tmp/alac-6.1-reference.s16
	${bindir}/sox${EXEEXT} /tmp/alac-6.1.wav /tmp/alac-6.1.m4a 2>/tmp/alac-6.1-encode.log
	grep -q 'Apple AAC 6.1 without remixing' /tmp/alac-6.1-encode.log ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/alac-6.1.m4a -t s16 /tmp/alac-6.1-decoded.s16
	cmp /tmp/alac-6.1-reference.s16 /tmp/alac-6.1-decoded.s16 ||
		exit 1

	echo "Format: m4a    Options: MPEG 7.1 B without automatic remix"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 8 -b 16 \
		/tmp/alac-7.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 110 sine 550 sine 660 sine 770 sine 880 vol .1
	${bindir}/sox${EXEEXT} /tmp/alac-7.1.wav -t s16 /tmp/alac-7.1-reference.s16
	${bindir}/sox${EXEEXT} /tmp/alac-7.1.wav /tmp/alac-7.1.m4a 2>/tmp/alac-7.1-encode.log
	grep -q 'MPEG 7.1 B without remixing' /tmp/alac-7.1-encode.log ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/alac-7.1.m4a -t s16 /tmp/alac-7.1-decoded.s16
	cmp /tmp/alac-7.1-reference.s16 /tmp/alac-7.1-decoded.s16 ||
		exit 1

	echo "Format: m4a    Options: explicit MPEG 7.1 B"
	${bindir}/sox${EXEEXT} --help-format m4a 2>/dev/null |
		grep -F '7.1(wide)          FL FR FC LFE BL BR FLC FRC' >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/alac-7.1.wav -b 16 \
		--channel-layout '7.1(wide)' /tmp/alac-7.1-explicit.m4a \
		2>/tmp/alac-7.1-explicit-encode.log
	if grep -q 'Encoding 8-channel ALAC' /tmp/alac-7.1-explicit-encode.log; then
		echo "Explicit ALAC layout produced a redundant warning"
		exit 1
	fi
	${bindir}/sox${EXEEXT} /tmp/alac-7.1-explicit.m4a -t s16 /tmp/alac-7.1-explicit-decoded.s16
	cmp /tmp/alac-7.1-reference.s16 /tmp/alac-7.1-explicit-decoded.s16 ||
		exit 1
	if ${bindir}/sox${EXEEXT} /tmp/alac-7.1.wav -b 16 \
		--channel-layout 7.1 /tmp/alac-invalid-layout.m4a \
		2>/dev/null; then
		echo "ALAC accepted a non-official 7.1 layout"
		exit 1
	fi
	test_alac_layout /tmp/alac-7.1.wav mono 1 1
	test_alac_layout /tmp/alac-7.1.wav stereo 2 2
	test_alac_layout /tmp/alac-7.1.wav 3.0 3 3
	test_alac_layout /tmp/alac-7.1.wav 4.0 4 4
	test_alac_layout /tmp/alac-7.1.wav 5.0 5 5
	test_alac_layout /tmp/alac-7.1.wav 5.1 6 6
	test_alac_layout /tmp/alac-7.1.wav '6.1(back)' 7 7
	test_alac_layout /tmp/alac-7.1.wav '7.1(wide)' 8 8
fi

if ${bindir}/sox${EXEEXT} --help-format aac 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: aac    Options: -C 128, stereo ADTS"
	${bindir}/sox${EXEEXT} -R -n -r 44100 -c 2 /tmp/aac-stereo.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-stereo.wav -C 128 /tmp/monkey.aac $effect
	${bindir}/sox${EXEEXT} /tmp/monkey.aac /tmp/monkey1.wav $effect

	echo "Format: aac    Options: -C 384, 6-channel ADTS"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 /tmp/aac-5.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 60 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-5.1.wav -C 384 /tmp/aac-5.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-5.1.aac /tmp/aac-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-5.1-decoded.wav`" = 6

	echo "Format: aac    Options: FFmpeg AVOptions passthrough"
	${bindir}/sox${EXEEXT} /tmp/aac-stereo.wav -C 128 --ffmpeg-opts aac_coder=fast:aac_pns=0 /tmp/aac-options.aac
	${bindir}/sox${EXEEXT} --ffmpeg-opts dual_mono_mode=both /tmp/aac-options.aac /tmp/aac-options-decoded.wav

	echo "Format: aac    Options: -C 256, quad PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 4 /tmp/aac-quad.wav synth .1 sine 220 sine 330 sine 440 sine 550 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-quad.wav -C 256 /tmp/aac-quad.aac
	${bindir}/sox${EXEEXT} /tmp/aac-quad.aac /tmp/aac-quad-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-quad-decoded.wav`" = 4 ||
		exit 1
	check_channel_frequencies /tmp/aac-quad-decoded.wav 220 330 440 550
	test_explicit_layout aac /tmp/aac-quad.wav 4.0 4-0 4 256
	grep -q 'without remixing' /tmp/aac-4-0-decode.log ||
		exit 1
	test_explicit_layout aac /tmp/aac-quad.wav quad quad 4 256

	echo "Format: aac    Options: -C 448, 6.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 7 /tmp/aac-6.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-6.1.wav -C 448 /tmp/aac-6.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-6.1.aac /tmp/aac-6.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-6.1-decoded.wav`" = 7 ||
		exit 1
	check_channel_frequencies /tmp/aac-6.1-decoded.wav 220 330 440 110 550 660 770
	test_explicit_layout aac /tmp/aac-6.1.wav 6.1 6-1 7 448
	test_explicit_layout aac /tmp/aac-6.1.wav '6.1(back)' 6-1-back 7 448

	echo "Format: aac    Options: -C 512, 7.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 8 /tmp/aac-7.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 sine 880 vol .1
	${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav -C 512 /tmp/aac-7.1.aac
	${bindir}/sox${EXEEXT} /tmp/aac-7.1.aac /tmp/aac-7.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/aac-7.1-decoded.wav`" = 8 ||
		exit 1
	check_channel_frequencies /tmp/aac-7.1-decoded.wav 220 330 440 110 550 660 770 880
	test_explicit_layout aac /tmp/aac-7.1.wav 7.1 7-1 8 512
	test_explicit_layout aac /tmp/aac-7.1.wav '7.1(wide)' 7-1-wide 8 512
	test_explicit_layout aac /tmp/aac-7.1.wav '7.1(wide-side)' 7-1-wide-side 8 512

	if ${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav \
		--channel-layout nonsense /tmp/aac-invalid-layout.aac \
		2>/dev/null; then
		echo "AAC accepted an unknown channel layout"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/aac-6.1.wav \
		--channel-layout '7.1(wide)' /tmp/aac-layout-mismatch.aac \
		2>/dev/null; then
		echo "AAC accepted a layout with the wrong channel count"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav \
		--channel-layout cube /tmp/aac-unsupported-layout.aac \
		2>/dev/null; then
		echo "AAC accepted a layout unsupported by its encoder"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav --channel-layout 7.1 /tmp/aac-layout.wav 2>/dev/null; then
		echo "A non-FFmpeg format accepted --channel-layout"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/aac-7.1.wav --ffmpeg-opts ch_layout=7.1 /tmp/aac-raw-layout.aac 2>/dev/null; then
		echo "A raw FFmpeg channel layout override was accepted"
		exit 1
	fi

	cp /tmp/monkey.aac /tmp/aac-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/aac-autodetect.bin /tmp/aac-autodetect.wav
fi

if ${bindir}/sox${EXEEXT} --help-format latm 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: latm   Options: -C 128, stereo LOAS/LATM"
	${bindir}/sox${EXEEXT} -R -n -r 44100 -c 2 /tmp/latm-stereo.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/latm-stereo.wav -C 128 /tmp/monkey.latm
	${bindir}/sox${EXEEXT} /tmp/monkey.latm /tmp/latm-stereo-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/latm-stereo-decoded.wav`" = 2 ||
		exit 1

	echo "Format: latm   Options: -C 256, quad PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 4 /tmp/latm-quad.wav \
		synth .1 sine 220 sine 330 sine 440 sine 550 vol .1
	${bindir}/sox${EXEEXT} /tmp/latm-quad.wav -C 256 /tmp/latm-quad.latm
	${bindir}/sox${EXEEXT} /tmp/latm-quad.latm /tmp/latm-quad-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/latm-quad-decoded.wav`" = 4 ||
		exit 1
	check_channel_frequencies /tmp/latm-quad-decoded.wav 220 330 440 550
	test_explicit_layout latm /tmp/latm-quad.wav 4.0 4-0 4 256
	grep -q 'without remixing' /tmp/latm-4-0-decode.log ||
		exit 1
	test_explicit_layout latm /tmp/latm-quad.wav quad quad 4 256

	echo "Format: latm   Options: -C 448, 6.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 7 /tmp/latm-6.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 vol .1
	${bindir}/sox${EXEEXT} /tmp/latm-6.1.wav -C 448 /tmp/latm-6.1.latm
	${bindir}/sox${EXEEXT} /tmp/latm-6.1.latm /tmp/latm-6.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/latm-6.1-decoded.wav`" = 7 ||
		exit 1
	check_channel_frequencies /tmp/latm-6.1-decoded.wav 220 330 440 110 550 660 770
	test_explicit_layout latm /tmp/latm-6.1.wav 6.1 6-1 7 448
	test_explicit_layout latm /tmp/latm-6.1.wav '6.1(back)' 6-1-back 7 448

	echo "Format: latm   Options: -C 512, 7.1 PCE"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 8 /tmp/latm-7.1.wav \
		synth .1 sine 220 sine 330 sine 440 sine 110 sine 550 sine 660 \
		sine 770 sine 880 vol .1
	${bindir}/sox${EXEEXT} /tmp/latm-7.1.wav -C 512 /tmp/latm-7.1.latm
	${bindir}/sox${EXEEXT} /tmp/latm-7.1.latm /tmp/latm-7.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/latm-7.1-decoded.wav`" = 8 ||
		exit 1
	check_channel_frequencies /tmp/latm-7.1-decoded.wav 220 330 440 110 550 660 770 880
	test_explicit_layout latm /tmp/latm-7.1.wav 7.1 7-1 8 512
	test_explicit_layout latm /tmp/latm-7.1.wav '7.1(wide)' 7-1-wide 8 512
	test_explicit_layout latm /tmp/latm-7.1.wav '7.1(wide-side)' 7-1-wide-side 8 512

	cp /tmp/monkey.latm /tmp/latm-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/latm-autodetect.bin /tmp/latm-autodetect.wav
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
	${bindir}/sox${EXEEXT} /tmp/ac3-autodetect.bin /tmp/ac3-autodetect.wav
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
	${bindir}/sox${EXEEXT} --ffmpeg-opts drc_scale=0.5 /tmp/eac3-options.eac3 /tmp/eac3-options-decoded.wav
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
	if ${bindir}/sox${EXEEXT} /tmp/eac3-5.1.wav --ffmpeg-opts dialnorm=-27 /tmp/eac3-options.wav 2>/dev/null; then
		echo "FFmpeg options were accepted by a non-FFmpeg format"
		exit 1
	fi

	cp /tmp/eac3-5.1.eac3 /tmp/eac3-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/eac3-autodetect.bin /tmp/eac3-autodetect.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/eac3-autodetect.wav`" = 6

	cp /tmp/eac3-5.1.eac3 /tmp/eac3-mislabeled.ac3
	cp /tmp/ac3-5.1.ac3 /tmp/ac3-mislabeled.eac3
	test "`${bindir}/sox${EXEEXT} --i -e /tmp/eac3-mislabeled.ac3`" = "ATSC A/52 E-AC-3"
	test "`${bindir}/sox${EXEEXT} --i -e /tmp/ac3-mislabeled.eac3`" = "ATSC A/52 AC-3"
fi

if ${bindir}/sox${EXEEXT} --help-format dts 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: dts    Options: -C 384, mono 44.1 kHz"
	${bindir}/sox${EXEEXT} -R -n -r 44100 -c 1 -b 16 /tmp/dts-mono.wav synth .1 sine 997 vol .1
	${bindir}/sox${EXEEXT} /tmp/dts-mono.wav -C 384 /tmp/dts-mono.dts
	${bindir}/sox${EXEEXT} /tmp/dts-mono.dts /tmp/dts-mono-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/dts-mono-decoded.wav`" = 1 ||
		exit 1
	test "`${bindir}/sox${EXEEXT} --i -r /tmp/dts-mono-decoded.wav`" = 44100 ||
		exit 1

	echo "Format: dts    Options: -C 768, stereo, pipe and autodetect"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 2 -b 24 /tmp/dts-stereo.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/dts-stereo.wav --ffmpeg-opts dca_adpcm=1 -C 768 /tmp/dts-stereo.dts
	cat /tmp/dts-stereo.dts |
		${bindir}/sox${EXEEXT} - /tmp/dts-pipe.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/dts-pipe.wav`" = 2 ||
		exit 1
	cp /tmp/dts-stereo.dts /tmp/dts-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/dts-autodetect.bin /tmp/dts-autodetect.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/dts-autodetect.wav`" = 2 ||
		exit 1

	echo "Format: dts    Options: -C 1536, 5.1(side)"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 -b 24 \
		/tmp/dts-5.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 60 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} --help-format dts 2>/dev/null |
		grep -F '5.1(side)          FL FR FC LFE SL SR' >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/dts-5.1.wav -C 1536 /tmp/dts-5.1-implicit.dts 2>/tmp/dts-5.1-implicit.log
	grep -F 'Encoding 6-channel DTS as 5.1(side)' /tmp/dts-5.1-implicit.log >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/dts-5.1.wav -C 1536 --channel-layout '5.1(side)' /tmp/dts-5.1.dts
	${bindir}/sox${EXEEXT} /tmp/dts-5.1.dts /tmp/dts-5.1-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/dts-5.1-decoded.wav`" = 6 ||
		exit 1
	check_channel_frequencies /tmp/dts-5.1-decoded.wav 220 330 440 60 550 660

	echo "Format: dts    Options: incomplete final encoder frame"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 2 -b 24 /tmp/dts-final.wav synth 1001s sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/dts-final.wav -C 768 /tmp/dts-final.dts
	${bindir}/sox${EXEEXT} /tmp/dts-final.dts /tmp/dts-final-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -s /tmp/dts-final-decoded.wav`" = 1024 ||
		exit 1

	if ${bindir}/sox${EXEEXT} /tmp/dts-stereo.wav \
		--ffmpeg-opts unknown_sox_test_option=1 \
		/tmp/dts-invalid-option.dts 2>/dev/null; then
		echo "DTS accepted an unknown FFmpeg option"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} --ffmpeg-opts channel_order=coded /tmp/dts-stereo.dts -n 2>/dev/null; then
		echo "DTS accepted a SoX-controlled channel-order option"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/dts-stereo.wav -C 31 /tmp/dts-invalid-low.dts 2>/dev/null; then
		echo "DTS accepted a bitrate below its supported range"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/dts-stereo.wav -C 3841 /tmp/dts-invalid-high.dts 2>/dev/null; then
		echo "DTS accepted a bitrate above its supported range"
		exit 1
	fi

	dd if=/tmp/dts-stereo.dts of=/tmp/dts-truncated.dts bs=1 count=10 2>/dev/null
	if ${bindir}/sox${EXEEXT} -t dts /tmp/dts-truncated.dts -n 2>/tmp/dts-truncated.log; then
		echo "DTS accepted a truncated first access unit"
		exit 1
	fi
	grep -F 'Unable to submit compressed audio packet' /tmp/dts-truncated.log >/dev/null ||
		exit 1
	dd if=/dev/zero of=/tmp/dts-corrupt.dts bs=2048 count=1 2>/dev/null
	if ${bindir}/sox${EXEEXT} -t dts /tmp/dts-corrupt.dts -n 2>/dev/null; then
		echo "DTS accepted corrupt input"
		exit 1
	fi

	${bindir}/sox${EXEEXT} --help-format dtshd 2>/dev/null |
		grep -F 'Writes: no' >/dev/null ||
		exit 1
	if ${bindir}/sox${EXEEXT} -t dtshd /tmp/dts-stereo.dts -n 2>/tmp/dts-core-as-hd.log; then
		echo "DTS-HD accepted a core-only stream"
		exit 1
	fi
	grep -F 'DTS core rather than DTS-HD' /tmp/dts-core-as-hd.log >/dev/null ||
		exit 1

fi

if ${bindir}/sox${EXEEXT} --help-format truehd 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: truehd Options: lossless 16-bit stereo and pipe"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 2 -b 16 /tmp/truehd-stereo.wav synth .1 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} -D /tmp/truehd-stereo.wav -t s16 /tmp/truehd-stereo-reference.s16
	${bindir}/sox${EXEEXT} /tmp/truehd-stereo.wav --ffmpeg-opts max_interval=8 /tmp/truehd-stereo.thd
	${bindir}/sox${EXEEXT} -D /tmp/truehd-stereo.thd -t s16 /tmp/truehd-stereo-decoded.s16
	cmp /tmp/truehd-stereo-reference.s16 /tmp/truehd-stereo-decoded.s16 ||
		exit 1
	cat /tmp/truehd-stereo.thd |
		${bindir}/sox${EXEEXT} - /tmp/truehd-pipe.wav
	test "`${bindir}/sox${EXEEXT} --i -c /tmp/truehd-pipe.wav`" = 2 ||
		exit 1
	cp /tmp/truehd-stereo.thd /tmp/truehd-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/truehd-autodetect.bin /tmp/truehd-autodetect.wav

	echo "Format: truehd Options: lossless 24-bit 5.1(side)"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 6 -b 24 \
		/tmp/truehd-5.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 110 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} -D /tmp/truehd-5.1.wav -t s32 /tmp/truehd-5.1-reference.s32
	${bindir}/sox${EXEEXT} --help-format truehd 2>/dev/null |
		grep -F '5.1(side)          FL FR FC LFE SL SR' >/dev/null ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/truehd-5.1.wav --channel-layout '5.1(side)' /tmp/truehd-5.1.thd
	${bindir}/sox${EXEEXT} -D /tmp/truehd-5.1.thd -t s32 /tmp/truehd-5.1-decoded.s32
	cmp /tmp/truehd-5.1-reference.s32 /tmp/truehd-5.1-decoded.s32 ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/truehd-5.1.thd /tmp/truehd-5.1-decoded.wav
	check_channel_frequencies /tmp/truehd-5.1-decoded.wav 220 330 440 110 550 660

	echo "Format: truehd Options: incomplete final encoder frame"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 2 -b 16 /tmp/truehd-final.wav synth 1001s sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} /tmp/truehd-final.wav /tmp/truehd-final.thd
	${bindir}/sox${EXEEXT} /tmp/truehd-final.thd /tmp/truehd-final-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -s /tmp/truehd-final-decoded.wav`" = 1001 ||
		exit 1

	if ${bindir}/sox${EXEEXT} /tmp/truehd-5.1.wav \
		-C 1 --channel-layout '5.1(side)' /tmp/truehd-invalid-C.thd \
		2>/dev/null; then
		echo "TrueHD accepted -C"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/truehd-5.1.wav \
		--ffmpeg-opts unknown_sox_test_option=1 \
		--channel-layout '5.1(side)' /tmp/truehd-invalid-option.thd \
		2>/dev/null; then
		echo "TrueHD accepted an unknown FFmpeg option"
		exit 1
	fi
	if ${bindir}/sox${EXEEXT} /tmp/truehd-5.1.wav /tmp/truehd-invalid-layout.thd 2>/dev/null; then
		echo "TrueHD accepted its unsupported canonical 5.1(back) default"
		exit 1
	fi

	truehd_size=`wc -c < /tmp/truehd-5.1.thd | tr -d ' '`
	dd if=/tmp/truehd-5.1.thd of=/tmp/truehd-truncated.thd bs=1 count=`expr "$truehd_size" - 1` 2>/dev/null
	${bindir}/sox${EXEEXT} /tmp/truehd-truncated.thd -n 2>/tmp/truehd-truncated.log
	grep -F 'Truncated Dolby TrueHD access unit' /tmp/truehd-truncated.log >/dev/null ||
		exit 1
fi

if ${bindir}/sox${EXEEXT} --help-format mlp 2>/dev/null | grep -q '^Writes:$'; then
	echo "Format: mlp    Options: lossless 24-bit 192 kHz stereo"
	${bindir}/sox${EXEEXT} -R -n -r 192000 -c 2 -b 24 /tmp/mlp-high-rate.wav synth .05 sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} -D /tmp/mlp-high-rate.wav -t s32 /tmp/mlp-high-rate-reference.s32
	${bindir}/sox${EXEEXT} /tmp/mlp-high-rate.wav --ffmpeg-opts max_interval=8 /tmp/mlp-high-rate.mlp
	${bindir}/sox${EXEEXT} -D /tmp/mlp-high-rate.mlp -t s32 /tmp/mlp-high-rate-decoded.s32
	cmp /tmp/mlp-high-rate-reference.s32 /tmp/mlp-high-rate-decoded.s32 ||
		exit 1
	cp /tmp/mlp-high-rate.mlp /tmp/mlp-autodetect.bin
	${bindir}/sox${EXEEXT} /tmp/mlp-autodetect.bin /tmp/mlp-autodetect.wav
	cat /tmp/mlp-high-rate.mlp |
		${bindir}/sox${EXEEXT} - /tmp/mlp-pipe.wav

	echo "Format: mlp    Options: lossless 24-bit 5.1"
	${bindir}/sox${EXEEXT} -R -n -r 96000 -c 6 -b 24 \
		/tmp/mlp-5.1.wav synth .1 sine 220 sine 330 sine 440 \
		sine 110 sine 550 sine 660 vol .1
	${bindir}/sox${EXEEXT} -D /tmp/mlp-5.1.wav -t s32 /tmp/mlp-5.1-reference.s32
	${bindir}/sox${EXEEXT} /tmp/mlp-5.1.wav --channel-layout 5.1 /tmp/mlp-5.1.mlp
	${bindir}/sox${EXEEXT} -D /tmp/mlp-5.1.mlp -t s32 /tmp/mlp-5.1-decoded.s32
	cmp /tmp/mlp-5.1-reference.s32 /tmp/mlp-5.1-decoded.s32 ||
		exit 1
	${bindir}/sox${EXEEXT} /tmp/mlp-5.1.mlp /tmp/mlp-5.1-decoded.wav
	check_channel_frequencies /tmp/mlp-5.1-decoded.wav 220 330 440 110 550 660

	echo "Format: mlp    Options: padded final encoder frame"
	${bindir}/sox${EXEEXT} -R -n -r 48000 -c 2 -b 16 /tmp/mlp-final.wav synth 1001s sine 997 sine 1499 vol .1
	${bindir}/sox${EXEEXT} -D /tmp/mlp-final.wav -t s16 /tmp/mlp-final-reference.s16
	${bindir}/sox${EXEEXT} /tmp/mlp-final.wav /tmp/mlp-final.mlp
	${bindir}/sox${EXEEXT} /tmp/mlp-final.mlp /tmp/mlp-final-decoded.wav
	test "`${bindir}/sox${EXEEXT} --i -s /tmp/mlp-final-decoded.wav`" = 1040 ||
		exit 1
	${bindir}/sox${EXEEXT} -D /tmp/mlp-final-decoded.wav -t s16 /tmp/mlp-final-trimmed.s16 trim 0 1001s
	cmp /tmp/mlp-final-reference.s16 /tmp/mlp-final-trimmed.s16 ||
		exit 1

	if ${bindir}/sox${EXEEXT} -t truehd /tmp/mlp-high-rate.mlp -n 2>/tmp/mlp-as-truehd.log; then
		echo "TrueHD accepted an MLP stream"
		exit 1
	fi
	grep -F 'The input is MLP rather than Dolby TrueHD' /tmp/mlp-as-truehd.log >/dev/null ||
		exit 1
	if ${bindir}/sox${EXEEXT} -t mlp /tmp/truehd-stereo.thd -n 2>/tmp/truehd-as-mlp.log; then
		echo "MLP accepted a TrueHD stream"
		exit 1
	fi
	grep -F 'The input is Dolby TrueHD rather than MLP' /tmp/truehd-as-mlp.log >/dev/null ||
		exit 1

	mlp_size=`wc -c < /tmp/mlp-high-rate.mlp | tr -d ' '`
	dd if=/tmp/mlp-high-rate.mlp of=/tmp/mlp-truncated.mlp bs=1 count=`expr "$mlp_size" - 1` 2>/dev/null
	${bindir}/sox${EXEEXT} /tmp/mlp-truncated.mlp -n 2>/tmp/mlp-truncated.log
	grep -F 'Truncated MLP access unit' /tmp/mlp-truncated.log >/dev/null ||
		exit 1
fi
