#! /bin/sh

# This is a shell script which runs a web browser in a home-container, complete with a simple UI
# profile manager constructed using Zenity. Some directories from your real homedir are shared
# with the browser:
#   Downloads (writable)
#   Uploads (read-only)
#   Pictures (read-only; this is my most common upload source)
#   .local/share (read-only)
#   .config/fontconfig (read-only)
#
# You need to specify the browser you want (e.g. "google-chrome", "chromium", "firefox") as
# a parameter to the script.
BROWSER=$1

LIST=$(cd ~/.home-container && ls -d browser-* | sed -e 's/^browser-//g')

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

exec home-container "browser-$PROFILE" --nx -w Downloads -r Uploads -r Pictures -r .local/share -r .config/fontconfig $BROWSER
