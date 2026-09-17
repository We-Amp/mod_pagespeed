#!/bin/bash
#
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
if ! "$SKIP_EXTERNAL_RESOURCE_TESTS"; then
  start_test inline_google_font_css before move_to_head and move_above_scripts
  URL="$TEST_ROOT/move_font_css_to_head.html"
  URL+="?PageSpeedFilters=inline_google_font_css,"
  URL+="move_css_to_head,move_css_above_scripts"
  # Make sure the font CSS stylesheet link tag is eliminated. We can't count
  # all <link> tags: the filter also inserts a fonts.gstatic.com preconnect
  # <link> that stays in the page.
  fetch_until -save $URL 'grep -c rel=\"stylesheet\"' 0
  # Check that we added fonts to the page.
  check [ $(fgrep -c '@font-face' $FETCH_FILE) -gt 0 ]
  # Check that exactly one preconnect hint was inserted for the font files.
  check [ $(fgrep -c 'rel="preconnect"' $FETCH_FILE) -eq 1 ]
  # Make sure last style line is before first script line.
  last_style=$(fgrep -n '<style>' $FETCH_FILE | tail -1 | grep -o '^[^:]*')
  first_script=$(\
    fgrep -n '<script>' $FETCH_FILE | tail -1 | grep -o '^[^:]*')
  check [ "$last_style" -lt "$first_script" ]
fi
