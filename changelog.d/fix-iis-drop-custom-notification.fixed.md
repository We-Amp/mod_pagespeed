- **IIS: requests no longer stall for one second when another module
  raises a custom notification.** The module subscribed to custom
  notifications and its handler slept one second on every one another
  module raised, although it never raises a custom notification itself;
  the subscription and the handler are gone.
