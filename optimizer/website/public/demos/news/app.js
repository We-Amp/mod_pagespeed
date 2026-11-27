// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Dispatch - News portal interactivity
 *
 * 1. Ticker: pauses CSS animation on hover
 * 2. Date: displays formatted date in the header
 * 3. Load More: appends additional news cards to the grid
 */

/* Format today's date as "Monday, January 15, 2025" */
function formatCurrentDate() {
  var today = new Date();
  var days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  var months = [
    'January',
    'February',
    'March',
    'April',
    'May',
    'June',
    'July',
    'August',
    'September',
    'October',
    'November',
    'December',
  ];
  return (
    days[today.getDay()] +
    ', ' +
    months[today.getMonth()] +
    ' ' +
    today.getDate() +
    ', ' +
    today.getFullYear()
  );
}

/* Display the date in the header element */
function displayDateInHeader() {
  var element = document.getElementById('header-date');
  if (element !== null) {
    element.textContent = formatCurrentDate();
  }
}

/* Pause the ticker animation when the mouse hovers over it */
function setupTickerHoverPause() {
  var ticker = document.getElementById('ticker');
  var track = document.getElementById('ticker-track');
  if (ticker === null || track === null) {
    return;
  }

  ticker.addEventListener('mouseenter', function () {
    track.style.animationPlayState = 'paused';
  });
  ticker.addEventListener('mouseleave', function () {
    track.style.animationPlayState = 'running';
  });
}

/* Extra stories to load when the button is clicked */
var extraStories = [
  {
    category: 'Politics',
    headline: 'Bipartisan Infrastructure Bill Passes Key Committee Vote',
    time: '8h ago',
    image: 'images/infrastructure.png',
  },
  {
    category: 'Technology',
    headline: 'Open Source AI Models Close Gap With Proprietary Systems',
    time: '9h ago',
    image: 'images/open-source-ai.png',
  },
  {
    category: 'World',
    headline: 'Historic Peace Agreement Signed Between Long-Standing Rivals',
    time: '10h ago',
    image: 'images/peace-agreement.jpg',
  },
];

var hasLoadedMore = false;

/* Build a news card DOM element from a story data object */
function createNewsCard(story) {
  var article = document.createElement('article');
  article.className = 'news-card';
  article.innerHTML =
    '<img src="' +
    story.image +
    '" alt="' +
    story.headline +
    '" class="news-card-image" width="300" height="200">' +
    '<div class="body">' +
    '<span class="cat">' +
    story.category +
    '</span>' +
    '<h3>' +
    story.headline +
    '</h3>' +
    '<span class="time">' +
    story.time +
    '</span>' +
    '</div>';
  return article;
}

/* Append additional news cards and disable the button */
function loadMoreStories() {
  if (hasLoadedMore) {
    return;
  }
  var grid = document.getElementById('news-grid');
  if (grid === null) {
    return;
  }

  for (var i = 0; i < extraStories.length; i++) {
    grid.appendChild(createNewsCard(extraStories[i]));
  }

  hasLoadedMore = true;
  var btn = document.getElementById('load-more-btn');
  if (btn !== null) {
    btn.textContent = "You're all caught up!";
    btn.style.opacity = '0.5';
    btn.style.cursor = 'default';
  }
}

/* Initialize all features on DOM ready */
document.addEventListener('DOMContentLoaded', function () {
  displayDateInHeader();
  setupTickerHoverPause();

  var btn = document.getElementById('load-more-btn');
  if (btn !== null) {
    btn.addEventListener('click', loadMoreStories);
  }
});
