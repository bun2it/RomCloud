// Theme: auto theo hệ thống, nhớ lựa chọn tay
(function () {
  var root = document.documentElement;
  function apply() {
    var t = localStorage.getItem("rc-theme") || "auto";
    if (t === "auto") root.removeAttribute("data-theme");
    else root.setAttribute("data-theme", t);
    var b = document.getElementById("themeBtn");
    if (b) b.textContent = "Giao diện: " + (t === "auto" ? "Tự động" : (t === "dark" ? "Tối" : "Sáng"));
  }
  window.rcThemeToggle = function () {
    var t = localStorage.getItem("rc-theme") || "auto";
    t = t === "auto" ? "dark" : (t === "dark" ? "light" : "auto");
    localStorage.setItem("rc-theme", t);
    apply();
  };
  document.addEventListener("DOMContentLoaded", apply);
  apply();
})();
