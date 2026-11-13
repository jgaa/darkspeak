# Desktop notifications

Settings → General → Enable desktop notifications controls all desktop
notifications. It defaults to on and takes effect when settings are saved.
It does not change the notifications shown inside DarkSpeak.

Each identity has two independent options in its Security preferences:

- Show sender names in desktop notifications (default off).
- Show message content and filenames in desktop notifications (default off).

Without these permissions, desktop notifications contain a generic event title
and a prompt to open DarkSpeak. Contact requests also hide the requester name
unless sender names are enabled, and never display the local identity name.
Missing identity preferences use the private defaults, including existing
identities. Changes apply to subsequent notifications; already delivered
desktop notifications are managed by the desktop environment.

The global setting is `notifications/enabled`. Identity settings are stored as
`identity/<uuid>/notificationShowSender` and
`identity/<uuid>/notificationShowContent`. These preferences stay local and are
not part of peer capabilities or the MCP API.
