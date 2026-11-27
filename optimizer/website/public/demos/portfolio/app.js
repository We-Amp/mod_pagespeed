// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Lena Voss Photography - Lightbox image viewer
 *
 * Click a gallery image to open it full-screen in a modal overlay.
 * Close via the X button, clicking the backdrop, or pressing Escape.
 */

var isLightboxOpen = false;

/* Open the lightbox with the given image URL and caption */
function openLightbox(imageSrc, captionText) {
  var modal = document.getElementById('lightbox');
  var image = document.getElementById('lb-img');
  var caption = document.getElementById('lb-caption');

  if (modal === null || image === null) {
    return;
  }

  image.setAttribute('src', imageSrc);
  image.setAttribute('alt', captionText);

  if (caption !== null) {
    caption.textContent = captionText;
  }

  modal.classList.add('active');
  document.body.style.overflow = 'hidden';
  isLightboxOpen = true;
}

/* Close the lightbox and restore scroll */
function closeLightbox() {
  var modal = document.getElementById('lightbox');
  var image = document.getElementById('lb-img');

  if (modal === null) {
    return;
  }

  modal.classList.remove('active');
  document.body.style.overflow = '';

  /* Clear image after fade to prevent stale flash */
  setTimeout(function () {
    if (image !== null) {
      image.setAttribute('src', '');
    }
  }, 300);

  isLightboxOpen = false;
}

/* Set up all event listeners on DOM ready */
document.addEventListener('DOMContentLoaded', function () {
  /* Gallery items: click to open lightbox */
  var items = document.querySelectorAll('.gallery-item');
  for (var i = 0; i < items.length; i++) {
    items[i].addEventListener('click', function () {
      var src = this.getAttribute('data-image');
      var title = this.getAttribute('data-title');
      if (src !== null) {
        openLightbox(src, title || '');
      }
    });
  }

  /* Close button */
  var closeBtn = document.getElementById('lb-close');
  if (closeBtn !== null) {
    closeBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      closeLightbox();
    });
  }

  /* Backdrop click to close */
  var modal = document.getElementById('lightbox');
  if (modal !== null) {
    modal.addEventListener('click', function (e) {
      if (e.target === modal) {
        closeLightbox();
      }
    });
  }

  /* Escape key to close */
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && isLightboxOpen) {
      closeLightbox();
    }
  });
});
