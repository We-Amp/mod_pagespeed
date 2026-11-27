// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * ByteWise Blog - Dark mode toggle and reading time calculator
 *
 * 1. Dark mode: toggles a CSS class on body, persists via localStorage
 * 2. Reading time: estimates minutes based on word count (200 wpm)
 */

/* Check if dark mode was previously enabled */
function isDarkModeEnabled() {
  var stored = localStorage.getItem('bytewise-dark-mode');
  return stored === 'true';
}

/* Apply or remove dark mode and update the toggle icon */
function applyDarkMode(shouldEnable) {
  var bodyElement = document.body;
  var iconElement = document.getElementById('dm-icon');

  if (shouldEnable) {
    bodyElement.classList.add('dark-mode');
    if (iconElement !== null) {
      iconElement.textContent = '\u2600'; /* Sun */
    }
  } else {
    bodyElement.classList.remove('dark-mode');
    if (iconElement !== null) {
      iconElement.textContent = '\u263D'; /* Moon */
    }
  }

  localStorage.setItem('bytewise-dark-mode', shouldEnable.toString());
}

/* Flip the dark mode state */
function toggleDarkMode() {
  var currentlyDark = document.body.classList.contains('dark-mode');
  applyDarkMode(!currentlyDark);
}

/* Calculate reading time from word count (200 words per minute) */
function calculateReadingTime(wordCount) {
  var averageSpeed = 200;
  var minutes = Math.ceil(wordCount / averageSpeed);
  if (minutes < 1) {
    minutes = 1;
  }
  return minutes;
}

/* Update all read-time elements based on their data-words attr */
function updateAllReadingTimes() {
  var elements = document.querySelectorAll('.read-time');
  for (var i = 0; i < elements.length; i++) {
    var words = elements[i].getAttribute('data-words');
    if (words !== null) {
      var minutes = calculateReadingTime(parseInt(words, 10));
      elements[i].textContent = minutes + ' min read';
    }
  }
}

/* Initialize on DOM ready */
document.addEventListener('DOMContentLoaded', function () {
  if (isDarkModeEnabled()) {
    applyDarkMode(true);
  }

  var toggleBtn = document.getElementById('dm-toggle');
  if (toggleBtn !== null) {
    toggleBtn.addEventListener('click', toggleDarkMode);
  }

  updateAllReadingTimes();
});
