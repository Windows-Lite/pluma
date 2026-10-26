// What Authenticator does on a code (src/components/Popup/EntryComponent.vue, copyCode): asks for the optional
// clipboardWrite permission, copies through a hidden input and execCommand, puts the focus back and stays open.
const out = document.getElementById('out');
function copy(then) {
  chrome.permissions.request({ permissions: ['clipboardWrite'] }, (granted) => {
    out.textContent = 'granted: ' + granted;
    if (!granted) return;
    const last = document.activeElement, clip = document.getElementById('clip');
    clip.value = '123456';
    clip.focus(); clip.select();
    out.textContent = 'copied: ' + document.execCommand('copy');
    last.focus();
    if (then) then();
  });
}
document.getElementById('copy').onclick = () => copy();
document.getElementById('copyclose').onclick = () => copy(() => window.close());
document.getElementById('close').onclick = () => window.close();
