#! /bin/sh

# This is a shell script which runs Google Chrome in a home-container, complete with a simple UI
# profile manager constructed using Zenity. Some directories from your real homedir are shared
# with chrome:
#   Downloads (writable)
#   Uploads (read-only)
#   Pictures (read-only; this is my most common upload source)
#   .local/share (read-only)
#   .config/fontconfig (read-only)

# You can specify an executable other than "google-chrome" (such as "chromium")
# as a command-line parameter to this script.
CHROME=${1:-google-chrome}

LIST=$(cd ~/.home-container && ls -d chrome-* | sed -e 's/^chrome-//g')

if [ "$LIST" = '*' ]; then
  LIST=
fi

PROFILE=$(zenity --list --title="Choose profile" --text= --width 256 --height 512 --column "Profile" $LIST "[new profile]")

if [ "$PROFILE" = "[new profile]" ]; then
  PROFILE=$(zenity --entry --title="New profile" --text="Profile name:")
fi

if [ -z "$PROFILE" ]; then
  exit 0
fi

exec home-container "chrome-$PROFILE" --nx -w Downloads -r Uploads -r Pictures -r .local/share -r .config/fontconfig $CHROME --user-data-dir=$HOME/.chrome
