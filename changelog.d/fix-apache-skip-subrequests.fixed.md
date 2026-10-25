- **Apache subrequests, such as server-side includes, no longer go through
  in-place optimization under the including page's URL.** The page and its
  included resources are handled only on their own requests.
