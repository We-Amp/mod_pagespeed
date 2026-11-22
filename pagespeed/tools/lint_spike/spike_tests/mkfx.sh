set -e
S="$(dirname "$0")/fx"
mkdir -p "$S/bad/conf.d"
cd "$S"
w(){ printf "%b" "$2" > "bad/$1.conf"; }
cat > expected.tsv <<'EOF'
01 PS101 2
02 PS102 2
03 PS103 2
04 PS103 2
05 PS201 2
06 PS202 2
07 PS201 2
08 PS201 2
09 PS301 3
10 PS302 3
11 PS303 3
12 PS304 1
13 PS304 1
14 PS305 3
15 PS306 2
16 PS307 2
17 PS102 1
18 PS901 2
19 PS902 1
20 PS903 1
21 PS104 1
22 PS102 2
23 PS303 3
EOF
w 01 "# unknown\nModPagespeedEnableFilterz rewrite_css\n"
w 02 "# invalid value\nModPagespeedRewriteLevel Bogus\n"
w 03 "# arity\nModPagespeedEnableFilters rewrite_css extra\n"
w 04 "# arity take2\nModPagespeedMapRewriteDomain onlyone\n"
w 05 "# deprecated filter\nModPagespeedEnableFilters insert_ga\n"
w 06 "# removed option\nModPagespeedDistributedRewriteServers host:1\n"
w 07 "# deprecated spelling\nModPagespeedImgInlineMaxBytes 2048\n"
w 08 "# deprecated option\nModPagespeedAnalyticsID UA-1\n"
w 09 "ModPagespeed on\n<Directory /var/www>\n  ModPagespeedFileCachePath /tmp/x\n</Directory>\n"
w 10 "ModPagespeed on\n<VirtualHost *:80>\n  ModPagespeedStrictAdminAccess on\n</VirtualHost>\n"
w 11 "ModPagespeed on\n<VirtualHost *:80>\n  ModPagespeedFetchWithGzip on\n</VirtualHost>\n"
w 12 "ModPagespeedCustomFetchHeader X-Test yes\n"
w 13 "ModPagespeedFileCachePath /tmp/x\n"
w 14 "<ModPagespeedIf spdy>\n  ModPagespeed on\n  <ModPagespeedIf !spdy>\n  </ModPagespeedIf>\n</ModPagespeedIf>\n"
w 15 "<ModPagespeedIf spdy>\n  ModPagespeedStrictAdminAccess on\n</ModPagespeedIf>\n"
w 16 "<ModPagespeedIf spdy>\n  Header set X-A b\n</ModPagespeedIf>\n"
w 17 "<ModPagespeedIf http2>\n</ModPagespeedIf>\n"
w 18 "ModPagespeed on\nInclude bad/conf.d/missing.conf\n"
w 19 "Include bad/conf.d/Sites.conf\n"
w 20 "Include ../../etc/passwd\n"
w 21 "<VirtualHost *:80>\n  ModPagespeed on\n"
w 22 "# bad on/off\nModPagespeed maybe\n"
w 23 "ModPagespeed on\n<VirtualHost *:80>\n  ModPagespeedForceCaching on\n</VirtualHost>\n"
printf "ModPagespeedEnableFilters rewrite_css\nModPagespeedBogus 1\n" > bad/conf.d/sites.conf
# A CRLF copy of a good config for the line-ending check.
printf "ModPagespeed on\r\nModPagespeedRewriteLevel CoreFilters\r\nModPagespeedEnableFilters insert_ga\r\n" > crlf.conf
ls bad | wc -l
