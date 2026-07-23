var retry = () => { try { return attempt(); } catch (e) { return null; } };
