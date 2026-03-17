function displayHandler() {
  return {
    lcdRotation: 4,
    jpegMirror: true,
    loading: false,
    message: "",

    init() {
      this.fetchConfig();
    },

    async fetchConfig() {
      this.loading = true;
      try {
        const res = await apiFetch("/api/v1/display/config");
        if (!res.ok) {
          this.message = "Failed to load config";
          return;
        }
        const data = await res.json();
        this.lcdRotation = data.lcd_rotation;
        this.jpegMirror = data.jpeg_mirror;
        this.message = "";
      } catch {
        this.message = "Error loading config";
      } finally {
        this.loading = false;
      }
    },

    async saveConfig() {
      this.loading = true;
      try {
        const res = await apiFetch("/api/v1/display/config", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            lcd_rotation: parseInt(this.lcdRotation, 10),
            jpeg_mirror: this.jpegMirror,
          }),
        });
        const data = await res.json();
        if (res.ok) {
          this.lcdRotation = data.lcd_rotation;
          this.jpegMirror = data.jpeg_mirror;
          this.message = "Saved. Reboot to apply rotation changes.";
        } else {
          this.message = data.message || "Failed to save";
        }
      } catch {
        this.message = "Error saving config";
      } finally {
        this.loading = false;
      }
    },
  };
}
