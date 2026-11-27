// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * ShopGrid - Cart counter and product filter
 *
 * Features:
 * 1. Shopping cart counter that tracks added items
 * 2. Category filter that shows/hides product cards
 *
 * This is a demo with no persistence - cart resets on reload.
 */

/* Current number of items in the cart */
var currentCartCount = 0;

/* Update the cart badge number in the nav */
function updateCartDisplay(newCount) {
  var cartCountElement = document.getElementById('cart-count');
  if (cartCountElement !== null) {
    cartCountElement.textContent = newCount.toString();
  }
}

/* Add a product to cart with visual feedback on the button */
function addItemToCart(productId, buttonElement) {
  currentCartCount = currentCartCount + 1;
  updateCartDisplay(currentCartCount);

  /* Visual feedback: green "Added!" for 1.2 seconds */
  var originalText = buttonElement.textContent;
  buttonElement.textContent = 'Added!';
  buttonElement.style.backgroundColor = '#27ae60';

  setTimeout(function () {
    buttonElement.textContent = originalText;
    buttonElement.style.backgroundColor = '';
  }, 1200);
}

/* Filter products by category, "all" shows everything */
function filterProductsByCategory(selectedCategory) {
  var allCards = document.querySelectorAll('.card');
  for (var i = 0; i < allCards.length; i++) {
    var cardCategory = allCards[i].getAttribute('data-category');
    if (selectedCategory === 'all' || cardCategory === selectedCategory) {
      allCards[i].style.display = 'block';
    } else {
      allCards[i].style.display = 'none';
    }
  }
}

/* Wire up event listeners when the DOM is ready */
document.addEventListener('DOMContentLoaded', function () {
  /* Add-to-cart buttons */
  var addButtons = document.querySelectorAll('.add-btn');
  for (var i = 0; i < addButtons.length; i++) {
    addButtons[i].addEventListener('click', function () {
      var productId = this.getAttribute('data-product-id');
      addItemToCart(productId, this);
    });
  }

  /* Filter buttons */
  var filterButtons = document.querySelectorAll('.filters button');
  for (var j = 0; j < filterButtons.length; j++) {
    filterButtons[j].addEventListener('click', function () {
      for (var k = 0; k < filterButtons.length; k++) {
        filterButtons[k].classList.remove('active');
      }
      this.classList.add('active');
      filterProductsByCategory(this.getAttribute('data-category'));
    });
  }
});
