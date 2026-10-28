#! /bin/sh
# Rebuild the documentation and upload this to the web server.
#
# it does use the Marek's implementation of the pod2html converter.
# You can find it from the CPAN, and I have a debian package of it on my
# my web page.
#
# Mt.

set -e # we want to fail on any error instead of risking uploading broken stuff
#set -x

function percent_lang {
	STATS=`msgfmt -o /dev/null --statistics po/pod/$1.po 2>&1`
	YES=`echo $STATS | sed -n -e 's/^\([[:digit:]]*\).*$/\1/p'`
	NO=`echo $STATS | sed -n -e 's/^\([[:digit:]]\+\)[^[:digit:]]\+\([[:digit:]]\+\).*$/\2/p'`
	if [ ! $NO ]; then
		NO=0
	fi
	O3=`echo $STATS | sed -n -e 's/^\([[:digit:]]\+\)[^[:digit:]]\+\([[:digit:]]\+\)[^[:digit:]]\+\([[:digit:]]\+\).*$/\3/p'`
	if [ $O3 ]; then
		NO=$(($NO + $O3))
	fi
	TOTAL=$(($YES+$NO))
	echo $((($YES*100)/$TOTAL))
}

#function get_name {
#	cat po/pod/$1.po | sed -n -e /\"NAME\"/,+1p | sed -n -e "s/msgstr \"\(.*\)\"/\1/p"
#}

function get_charset {
	case $1 in
		pl)
			echo "ISO-8859-2"
			;;
		*)
			echo "ISO-8859-1"
			;;
	esac
}

POFILES=`cd po/pod; ls *.po`
LANGS=${POFILES//.po/}

PO4A_OPTS="-k 0 -v -f pod -M utf-8"

./Build man

rm -rf html/
cp -a blib/man html
mkdir -p html/en/
mv html/man* html/en/
mkdir html/en/man3pm

# Generate the English man pages
find lib -name "*.pm" | while read file
do
	name=$(basename $file)
	name=${name//.pm/}
	pod2man --section=3pm --release="Po4a Tools" --center="Po4a Tools" \
	    $file html/en/man3pm/Locale::Po4a::$name.3pm
done
for file in po4a po4a-gettextize po4a-normalize po4a-translate po4a-updatepo
do
	pod2man --section=1 --release="Po4a Tools" --center="Po4a Tools" \
	    $file html/en/man1/$file.1
done

# Main page
echo "<html>
 <head>
  <title>po4a</title>
 </head>
 <body>
  <center>
   <h1>po4a</h1>
   <p>
    The po4a (po for anything) project goal is to ease translations (and
    more interestingly, the maintenance of translations) using gettext tools
    on areas where they were not expected like documentation.
   </p>
   <br>
   English documentation:
   <a href=\"en/man7/po4a.7.html\">Introduction</a>
   <a href=\"en\">Index</a>
   <br>
   <br>
   Documentation translations:
   <br>" > html/index.html
for lang in $LANGS ; do
	PERC=`percent_lang $lang`
	echo "   $lang ($PERC% translated):
   <a href=\"$lang/man7/po4a.7.html\">Introduction</a>
   <a href=\"$lang/\">Index</a>
   <br>" >> html/index.html
done
echo "   <br>
   <a href=\"http://alioth.debian.org/projects/po4a/\">Alioth project page</a>" >> html/index.html
echo "   <br>
   <a href=\"http://packages.qa.debian.org/po4a\">
    Debian developer information
   </a>" >> html/index.html
echo "   <br>
   <br>
   Last update: `LANG=C date`
  </center>
 </body>
</html>" >> html/index.html

for lang in en $LANGS ; do
	echo Generate the $lang index
	[ -d html/$lang/man3 ] && mv html/$lang/man3 html/$lang/man3pm

	echo "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0 Transitional//EN\">
<html>
 <head>
  <meta content=\"text/html; charset=$(get_charset $lang)\" http-equiv=\"Content-Type\">
  <title>Table of Contents</title>
 </head>
 <body>
  <h1>Table of Contents</h1>
  <hr>
   <table>" > html/$lang/index.html
	for man in html/$lang/man*/*
	do
		title=$(lexgrog "$man" | sed -ne 's/.*: \".* - //;s/"$//;p')
		man=$(echo $man | sed -e "s/^html\/$lang\///")
		man=${man%.gz}
		man=${man/1p/1}
		ref=$man.html
		man=$(basename $man)
		man=$(echo $man | sed -e 's/^\(.*\)\.\([0-9]\(pm\)\?\)$/\1(\2)/')
		echo "    <tr>
     <td><a href=\"$ref\">$man</a></td>
     <td>$title</td>
     </td>
    </tr>" >> html/$lang/index.html
	done
	echo "   </table>
  <hr>
 </body>
</html>" >> html/$lang/index.html

	echo Generate the $lang HTML pages
	for man in html/$lang/man*/*
	do
		if [ "$man" != "${man%.gz}" ]
		then
			gunzip $man
			man=${man%.gz}
		fi
		out=${man/1p/1}.html
		man2html -r $man | sed -e '/Content-type: text.html/d' \
		                       -e '/cgi-bin.man.man2html/d' \
		                       -e "s/<HEAD>/<HEAD><meta content=\"text\/html; charset=$(get_charset $lang)\" http-equiv=\"Content-Type\">/" > $out
		rm -f $man
	done
done


scp -pr html/* po4a.alioth.debian.org:/var/lib/gforge/chroot/home/groups/po4a/htdocs/
ssh po4a.alioth.debian.org chgrp -R po4a /var/lib/gforge/chroot/home/groups/po4a/htdocs/
ssh po4a.alioth.debian.org chmod -R g+rw /var/lib/gforge/chroot/home/groups/po4a/htdocs/
