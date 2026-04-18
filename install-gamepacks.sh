#!/bin/sh

: ${ECHO:=echo}
: ${SH:=sh}
: ${CP:=cp}
: ${CP_R:=cp -r}
: ${PACKFILTER:=}

dest=$1

case "$DOWNLOAD_GAMEPACKS" in
	yes)
		LICENSEFILTER=GPL BATCH=1 $SH download-gamepacks.sh
		;;
	q3rally)
		PACKFILTER="${PACKFILTER:-Q3RallyPack}" BATCH=1 $SH download-gamepacks.sh
		;;
	allinone)
		LICENSEFILTER=allinone BATCH=1 $SH download-gamepacks.sh
		;;
	all)
		BATCH=1 $SH download-gamepacks.sh
		;;
	*)
		;;
esac

set -e
for GAME in games/*Pack; do
	if [ "$GAME" = "games/*Pack" ]; then
		$ECHO "Game packs not found, please run"
		$ECHO "  ./download-gamepacks.sh"
		$ECHO "and then try again!"
	else
		PACK=${GAME##*/}
		case " $PACKFILTER " in
			"  ")
				;;
			*" $PACK "*)
				;;
			*)
				$ECHO "Skipping $PACK because it is not in PACKFILTER."
				continue
				;;
		esac
		$SH install-gamepack.sh "$GAME" "$dest"
	fi
done
