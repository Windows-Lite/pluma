// Pluma: the interface's language. The code says everything in Spanish (the person's own); T() gives it in the language
// chosen in the menu, or Windows's by default. A text missing from the table shows in Spanish.
#include "pluma.h"
#include <unordered_map>

static const wchar_t *kCodes[] = { L"es", L"en", L"pt", L"fr", L"de", L"it" };
struct Text { const wchar_t *es, *en, *pt, *fr, *de, *it; };
static const Text kTexts[] = {
    // the bar and the new tab page
    { L"Pestaña nueva", L"New tab", L"Nova guia", L"Nouvel onglet", L"Neuer Tab", L"Nuova scheda" },
    { L"Busca o escribe una dirección", L"Search or type an address", L"Pesquise ou digite um endereço", L"Recherchez ou saisissez une adresse", L"Suchen oder Adresse eingeben", L"Cerca o digita un indirizzo" },
    { L"Escribe una dirección arriba, o lo que quieras buscar.", L"Type an address above, or whatever you want to search for.", L"Digite um endereço acima, ou o que quiser pesquisar.", L"Saisissez une adresse ci-dessus, ou ce que vous voulez chercher.", L"Gib oben eine Adresse ein oder wonach du suchen willst.", L"Digita un indirizzo qui sopra, o quello che vuoi cercare." },
    { L"Tus páginas", L"Your pages", L"Suas páginas", L"Vos pages", L"Deine Seiten", L"Le tue pagine" },
    { L"Lo que más visitas", L"Most visited", L"Mais visitados", L"Les plus visités", L"Meistbesucht", L"Più visitati" },
    { L"Añade tus páginas con la estrella de la barra (Ctrl+D).", L"Add your pages with the star in the bar (Ctrl+D).", L"Adicione suas páginas com a estrela da barra (Ctrl+D).", L"Ajoutez vos pages avec l'étoile de la barre (Ctrl+D).", L"Füge deine Seiten mit dem Stern in der Leiste hinzu (Strg+D).", L"Aggiungi le tue pagine con la stella della barra (Ctrl+D)." },
    { L"¿Solo las tuyas? Añádelas con la estrella de la barra (Ctrl+D).", L"Only yours? Add them with the star in the bar (Ctrl+D).", L"Só as suas? Adicione-as com a estrela da barra (Ctrl+D).", L"Seulement les vôtres ? Ajoutez-les avec l'étoile de la barre (Ctrl+D).", L"Nur deine eigenen? Füge sie mit dem Stern in der Leiste hinzu (Strg+D).", L"Solo le tue? Aggiungile con la stella della barra (Ctrl+D)." },
    { L"Pluma usa ahora ", L"Pluma is using ", L"Pluma está usando ", L"Pluma utilise ", L"Pluma verwendet gerade ", L"Pluma sta usando " },
    { L" MB de memoria", L" MB of memory", L" MB de memória", L" Mo de mémoire", L" MB Speicher", L" MB di memoria" },
    { L"buscar con ", L"search with ", L"pesquisar com ", L"rechercher avec ", L"suchen mit ", L"cerca con " },
    { L"ir a la dirección", L"go to address", L"ir para o endereço", L"aller à l'adresse", L"Adresse öffnen", L"vai all'indirizzo" },
    { L"Quitada de tus páginas.", L"Removed from your pages.", L"Removida das suas páginas.", L"Retirée de vos pages.", L"Aus deinen Seiten entfernt.", L"Rimossa dalle tue pagine." },
    { L"No volverá a salir aquí.", L"It won't show up here again.", L"Não aparecerá mais aqui.", L"Elle n'apparaîtra plus ici.", L"Wird hier nicht mehr angezeigt.", L"Non apparirà più qui." },
    { L"Deshacer", L"Undo", L"Desfazer", L"Annuler", L"Rückgängig", L"Annulla" },
    // the menu
    { L"Reabrir pestaña cerrada", L"Reopen closed tab", L"Reabrir guia fechada", L"Rouvrir l'onglet fermé", L"Geschlossenen Tab öffnen", L"Riapri scheda chiusa" },
    { L"Ctrl+Mayús+T", L"Ctrl+Shift+T", L"Ctrl+Shift+T", L"Ctrl+Maj+T", L"Strg+Umschalt+T", L"Ctrl+Maiusc+T" },
    { L"Ctrl+T", L"Ctrl+T", L"Ctrl+T", L"Ctrl+T", L"Strg+T", L"Ctrl+T" },
    { L"Ctrl+F", L"Ctrl+F", L"Ctrl+F", L"Ctrl+F", L"Strg+F", L"Ctrl+F" },
    { L"Ctrl+P", L"Ctrl+P", L"Ctrl+P", L"Ctrl+P", L"Strg+P", L"Ctrl+P" },
    { L"Ctrl+J", L"Ctrl+J", L"Ctrl+J", L"Ctrl+J", L"Strg+J", L"Ctrl+J" },
    { L"Ctrl+W", L"Ctrl+W", L"Ctrl+W", L"Ctrl+W", L"Strg+W", L"Ctrl+W" },
    { L"Ctrl+Mayús+Q", L"Ctrl+Shift+Q", L"Ctrl+Shift+Q", L"Ctrl+Maj+Q", L"Strg+Umschalt+Q", L"Ctrl+Maiusc+Q" },
    { L"Buscar en la página", L"Find in page", L"Localizar na página", L"Rechercher dans la page", L"Auf Seite suchen", L"Trova nella pagina" },
    { L"Imprimir", L"Print", L"Imprimir", L"Imprimer", L"Drucken", L"Stampa" },
    { L"Descargas", L"Downloads", L"Downloads", L"Téléchargements", L"Downloads", L"Download" },
    { L"Extensiones", L"Extensions", L"Extensões", L"Extensions", L"Erweiterungen", L"Estensioni" },
    { L"1 activa", L"1 on", L"1 ativa", L"1 active", L"1 aktiv", L"1 attiva" },
    { L" activas", L" on", L" ativas", L" actives", L" aktiv", L" attive" },
    { L"1 activo", L"1 on", L"1 ativo", L"1 actif", L"1 aktiv", L"1 attivo" },
    { L" activos", L" on", L" ativos", L" actifs", L" aktiv", L" attivi" },
    { L"Scripts de usuario", L"User scripts", L"Scripts de usuário", L"Scripts utilisateur", L"Benutzerskripte", L"Script utente" },
    { L"Buscador", L"Search engine", L"Mecanismo de pesquisa", L"Moteur de recherche", L"Suchmaschine", L"Motore di ricerca" },
    { L"Tema", L"Theme", L"Tema", L"Thème", L"Design", L"Tema" },
    { L"Claro", L"Light", L"Claro", L"Clair", L"Hell", L"Chiaro" },
    { L"Oscuro", L"Dark", L"Escuro", L"Sombre", L"Dunkel", L"Scuro" },
    { L"Como Windows", L"Same as Windows", L"Igual ao Windows", L"Comme Windows", L"Wie Windows", L"Come Windows" },
    { L"Fuente", L"Font", L"Fonte", L"Police", L"Schriftart", L"Carattere" },
    { L"La de Windows", L"Windows default", L"A do Windows", L"Celle de Windows", L"Windows-Standard", L"Quello di Windows" },
    { L"Idioma", L"Language", L"Idioma", L"Langue", L"Sprache", L"Lingua" },
    { L"YouTube en el reproductor de Pluma", L"YouTube in Pluma's player", L"YouTube no player do Pluma", L"YouTube dans le lecteur de Pluma", L"YouTube im Pluma-Player", L"YouTube nel lettore di Pluma" },
    { L"Sugerir lo que más visito", L"Suggest my most visited", L"Sugerir os mais visitados", L"Suggérer les plus visités", L"Meistbesuchte vorschlagen", L"Suggerisci i più visitati" },
    { L"Borrar datos", L"Clear data", L"Limpar dados", L"Effacer les données", L"Daten löschen", L"Cancella dati" },
    { L"Herramientas de desarrollo", L"Developer tools", L"Ferramentas do desenvolvedor", L"Outils de développement", L"Entwicklertools", L"Strumenti per sviluppatori" },
    { L"Salir de Pluma", L"Quit Pluma", L"Sair do Pluma", L"Quitter Pluma", L"Pluma beenden", L"Esci da Pluma" },
    { L"Todavía no hay ninguna.", L"None yet.", L"Nenhuma ainda.", L"Aucune pour l'instant.", L"Noch keine.", L"Ancora nessuna." },
    { L"Todavía no hay ninguno.", L"None yet.", L"Nenhum ainda.", L"Aucun pour l'instant.", L"Noch keine.", L"Ancora nessuno." },
    { L"Buscar en la Chrome Web Store", L"Browse the Chrome Web Store", L"Procurar na Chrome Web Store", L"Parcourir le Chrome Web Store", L"Im Chrome Web Store suchen", L"Cerca nel Chrome Web Store" },
    { L"Abrir la carpeta de extensiones", L"Open the extensions folder", L"Abrir a pasta de extensões", L"Ouvrir le dossier des extensions", L"Erweiterungsordner öffnen", L"Apri la cartella delle estensioni" },
    { L"En la tienda, abre una extensión y pulsa «Añadir a Pluma».", L"In the store, open an extension and press \x201c" L"Add to Pluma\x201d.", L"Na loja, abra uma extensão e clique em \x201c" L"Adicionar ao Pluma\x201d.", L"Dans la boutique, ouvrez une extension et cliquez sur « Ajouter à Pluma ».", L"Öffne im Store eine Erweiterung und klicke auf \x201e" L"Zu Pluma hinzufügen\x201c.", L"Nello store, apri un'estensione e premi «Aggiungi a Pluma»." },
    { L"Abrir la carpeta de scripts", L"Open the scripts folder", L"Abrir a pasta de scripts", L"Ouvrir le dossier des scripts", L"Skriptordner öffnen", L"Apri la cartella degli script" },
    { L"Abre un archivo .user.js en Pluma para instalarlo.", L"Open a .user.js file in Pluma to install it.", L"Abra um arquivo .user.js no Pluma para instalá-lo.", L"Ouvrez un fichier .user.js dans Pluma pour l'installer.", L"Öffne eine .user.js-Datei in Pluma, um sie zu installieren.", L"Apri un file .user.js in Pluma per installarlo." },
    { L"El historial de Pluma", L"Pluma's history", L"O histórico do Pluma", L"L'historique de Pluma", L"Den Pluma-Verlauf", L"La cronologia di Pluma" },
    { L"Historial borrado", L"History cleared", L"Histórico limpo", L"Historique effacé", L"Verlauf gelöscht", L"Cronologia cancellata" },
    { L"Todo menos las contraseñas", L"Everything but passwords", L"Tudo menos as senhas", L"Tout sauf les mots de passe", L"Alles außer Passwörtern", L"Tutto tranne le password" },
    { L"Datos borrados: cookies, caché e historial", L"Data cleared: cookies, cache and history", L"Dados limpos: cookies, cache e histórico", L"Données effacées : cookies, cache et historique", L"Daten gelöscht: Cookies, Cache und Verlauf", L"Dati cancellati: cookie, cache e cronologia" },
    { L"Las cookies cierran tus sesiones en las webs.", L"Clearing cookies signs you out of websites.", L"Limpar os cookies encerra suas sessões nos sites.", L"Effacer les cookies vous déconnecte des sites.", L"Beim Löschen der Cookies wirst du auf Websites abgemeldet.", L"Cancellare i cookie ti disconnette dai siti." },
    { L"Recargar", L"Reload", L"Recarregar", L"Actualiser", L"Neu laden", L"Ricarica" },
    { L"Duplicar", L"Duplicate", L"Duplicar", L"Dupliquer", L"Duplizieren", L"Duplica" },
    { L"Volver a dar sonido", L"Unmute", L"Ativar som", L"Réactiver le son", L"Ton an", L"Riattiva audio" },
    { L"Silenciar", L"Mute", L"Silenciar", L"Couper le son", L"Stummschalten", L"Disattiva audio" },
    { L"Cerrar las de la derecha", L"Close tabs to the right", L"Fechar guias à direita", L"Fermer les onglets à droite", L"Tabs rechts schließen", L"Chiudi schede a destra" },
    { L"Cerrar las demás", L"Close other tabs", L"Fechar outras guias", L"Fermer les autres onglets", L"Andere Tabs schließen", L"Chiudi altre schede" },
    { L"Cerrar", L"Close", L"Fechar", L"Fermer", L"Schließen", L"Chiudi" },
    // pages, extensions, scripts
    { L"No se pudo iniciar el motor de Edge (WebView2). Instala «Microsoft Edge WebView2 Runtime».", L"Couldn't start Edge's engine (WebView2). Install \x201cMicrosoft Edge WebView2 Runtime\x201d.", L"Não foi possível iniciar o motor do Edge (WebView2). Instale o \x201cMicrosoft Edge WebView2 Runtime\x201d.", L"Impossible de démarrer le moteur d'Edge (WebView2). Installez « Microsoft Edge WebView2 Runtime ».", L"Die Edge-Engine (WebView2) konnte nicht starten. Installiere \x201eMicrosoft Edge WebView2 Runtime\x201c.", L"Impossibile avviare il motore di Edge (WebView2). Installa «Microsoft Edge WebView2 Runtime»." },
    { L"Esta extensión se puede añadir a Pluma.", L"This extension can be added to Pluma.", L"Esta extensão pode ser adicionada ao Pluma.", L"Cette extension peut être ajoutée à Pluma.", L"Diese Erweiterung kann zu Pluma hinzugefügt werden.", L"Questa estensione può essere aggiunta a Pluma." },
    { L"Añadir a Pluma", L"Add to Pluma", L"Adicionar ao Pluma", L"Ajouter à Pluma", L"Zu Pluma hinzufügen", L"Aggiungi a Pluma" },
    { L"Esta página es un script de usuario. ¿Lo instalas en Pluma?", L"This page is a user script. Install it in Pluma?", L"Esta página é um script de usuário. Instalar no Pluma?", L"Cette page est un script utilisateur. L'installer dans Pluma ?", L"Diese Seite ist ein Benutzerskript. In Pluma installieren?", L"Questa pagina è uno script utente. Installarlo in Pluma?" },
    { L"Instalar", L"Install", L"Instalar", L"Installer", L"Installieren", L"Installa" },
    { L"Script instalado: ", L"Script installed: ", L"Script instalado: ", L"Script installé : ", L"Skript installiert: ", L"Script installato: " },
    { L"No parece un script de usuario válido", L"This doesn't look like a valid user script", L"Não parece um script de usuário válido", L"Ce n'est pas un script utilisateur valide", L"Das ist kein gültiges Benutzerskript", L"Non sembra uno script utente valido" },
    { L"Abrir en una pestaña nueva", L"Open in new tab", L"Abrir em nova guia", L"Ouvrir dans un nouvel onglet", L"In neuem Tab öffnen", L"Apri in una nuova scheda" },
    { L"Extensión quitada: ", L"Extension removed: ", L"Extensão removida: ", L"Extension supprimée : ", L"Erweiterung entfernt: ", L"Estensione rimossa: " },
    { L"Descargando la extensión...", L"Downloading the extension...", L"Baixando a extensão...", L"Téléchargement de l'extension...", L"Erweiterung wird heruntergeladen...", L"Download dell'estensione..." },
    { L"No se pudo descargar la extensión", L"Couldn't download the extension", L"Não foi possível baixar a extensão", L"Impossible de télécharger l'extension", L"Erweiterung konnte nicht heruntergeladen werden", L"Impossibile scaricare l'estensione" },
    { L"Añadida, pero es de las antiguas (MV2): puede que no funcione", L"Added, but it's an old one (MV2): it may not work", L"Adicionada, mas é das antigas (MV2): pode não funcionar", L"Ajoutée, mais c'est une ancienne (MV2) : elle risque de ne pas fonctionner", L"Hinzugefügt, aber veraltet (MV2): funktioniert vielleicht nicht", L"Aggiunta, ma è di quelle vecchie (MV2): potrebbe non funzionare" },
    { L"Extensión añadida", L"Extension added", L"Extensão adicionada", L"Extension ajoutée", L"Erweiterung hinzugefügt", L"Estensione aggiunta" },
    // incognito
    { L"Pestaña de incógnito", L"Incognito tab", L"Guia anônima", L"Onglet de navigation privée", L"Inkognito-Tab", L"Scheda in incognito" },
    { L"Abrir en una pestaña de incógnito", L"Open in incognito tab", L"Abrir em guia anônima", L"Ouvrir dans un onglet de navigation privée", L"In Inkognito-Tab öffnen", L"Apri in una scheda in incognito" },
    { L"Ctrl+Mayús+N", L"Ctrl+Shift+N", L"Ctrl+Shift+N", L"Ctrl+Maj+N", L"Strg+Umschalt+N", L"Ctrl+Maiusc+N" },
    { L"Incógnito", L"Incognito", L"Anônima", L"Privé", L"Inkognito", L"Incognito" },
    { L"incógnito", L"incognito", L"anônima", L"navigation privée", L"inkognito", L"incognito" },
    { L"Pluma no guarda el historial ni las cookies de estas pestañas:", L"Pluma doesn't keep the history or cookies of these tabs:", L"O Pluma não guarda o histórico nem os cookies destas guias:", L"Pluma ne garde ni l'historique ni les cookies de ces onglets :", L"Pluma speichert weder Verlauf noch Cookies dieser Tabs:", L"Pluma non salva la cronologia né i cookie di queste schede:" },
    { L"todo se borra al cerrar la última o al cerrar Pluma.", L"it's all deleted when you close the last one or Pluma.", L"tudo é apagado ao fechar a última ou o Pluma.", L"tout est effacé en fermant le dernier ou Pluma.", L"alles wird gelöscht, wenn du den letzten oder Pluma schließt.", L"tutto si cancella chiudendo l'ultima o Pluma." },
    { L"Las descargas y tus páginas sí se quedan, y las webs y tu red", L"Downloads and your pages do stay, and websites and your network", L"Os downloads e suas páginas ficam, e os sites e sua rede", L"Les téléchargements et vos pages restent, et les sites et votre réseau", L"Downloads und deine Seiten bleiben, und Websites und dein Netzwerk", L"I download e le tue pagine restano, e i siti e la tua rete" },
    { L"siguen viendo lo que visitas.", L"can still see what you visit.", L"continuam vendo o que você visita.", L"voient toujours ce que vous visitez.", L"sehen weiterhin, was du besuchst.", L"vedono comunque cosa visiti." },
    // an extension's own menu
    { L"Opciones", L"Options", L"Opções", L"Options", L"Optionen", L"Opzioni" },
    { L"Desactivar", L"Turn off", L"Desativar", L"Désactiver", L"Deaktivieren", L"Disattiva" },
    { L"Quitar de Pluma", L"Remove from Pluma", L"Remover do Pluma", L"Retirer de Pluma", L"Aus Pluma entfernen", L"Rimuovi da Pluma" },
    { L"Vuelve a activarla en ⋯ > Extensiones.", L"Turn it back on in ⋯ > Extensions.", L"Reative em ⋯ > Extensões.", L"Réactivez-la dans ⋯ > Extensions.", L"Wieder einschalten unter ⋯ > Erweiterungen.", L"Riattivala in ⋯ > Estensioni." },
    { L"Volver a poner uBlock Origin Lite", L"Bring back uBlock Origin Lite", L"Restaurar o uBlock Origin Lite", L"Remettre uBlock Origin Lite", L"uBlock Origin Lite wiederherstellen", L"Ripristina uBlock Origin Lite" },
    { L"Extensión añadida de nuevo", L"Extension added back", L"Extensão adicionada de novo", L"Extension rajoutée", L"Erweiterung wieder hinzugefügt", L"Estensione aggiunta di nuovo" },
    // Pluma's video player (as YouTube says it in each language)
    { L"Volver a reproducir", L"Replay", L"Assistir novamente", L"Revoir", L"Wiederholen", L"Riguarda" },
    { L"Reproducir", L"Play", L"Assistir", L"Lire", L"Wiedergabe", L"Riproduci" },
    { L"Pausa", L"Pause", L"Pausar", L"Pause", L"Pausieren", L"Metti in pausa" },
    { L"Activar sonido", L"Unmute", L"Ativar som", L"Réactiver le son", L"Stummschaltung aufheben", L"Riattiva audio" },
    { L"Subtítulos", L"Subtitles", L"Legendas", L"Sous-titres", L"Untertitel", L"Sottotitoli" },
    { L"Configuración", L"Settings", L"Configurações", L"Paramètres", L"Einstellungen", L"Impostazioni" },
    { L"Página de YouTube", L"YouTube page", L"Página do YouTube", L"Page YouTube", L"YouTube-Seite", L"Pagina di YouTube" },
    { L"Salir de pantalla completa", L"Exit full screen", L"Sair da tela inteira", L"Quitter le plein écran", L"Vollbildmodus beenden", L"Esci da schermo intero" },
    { L"Pantalla completa", L"Full screen", L"Tela inteira", L"Plein écran", L"Vollbild", L"Schermo intero" },
    { L"Desactivados", L"Off", L"Desativadas", L"Désactivés", L"Aus", L"Disattivati" },
    { L"Velocidad de reproducción", L"Playback speed", L"Velocidade da reprodução", L"Vitesse de lecture", L"Wiedergabegeschwindigkeit", L"Velocità di riproduzione" },
    { L"Calidad", L"Quality", L"Qualidade", L"Qualité", L"Qualität", L"Qualità" },
    { L"Automática (", L"Auto (", L"Automática (", L"Automatique (", L"Automatisch (", L"Automatica (" },
    { L"Automática", L"Auto", L"Automática", L"Automatique", L"Automatisch", L"Automatica" },
    { L"Normal", L"Normal", L"Normal", L"Normale", L"Standard", L"Normale" },
};

// the language's code: the one chosen, else Windows's (if Pluma has it), else English
const wchar_t *lang_code() {   // (always one of kCodes: T() tells them apart by address)
    for (auto c : kCodes) if (g_set.lang == c) return c;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_SPANISH: return kCodes[0];
    case LANG_PORTUGUESE: return kCodes[2];
    case LANG_FRENCH: return kCodes[3];
    case LANG_GERMAN: return kCodes[4];
    case LANG_ITALIAN: return kCodes[5];
    default: return kCodes[1];
    }
}
const wchar_t *T(const wchar_t *es) {
    static std::unordered_map<std::wstring, const wchar_t *> table;
    static const wchar_t *built = nullptr;
    const wchar_t *code = lang_code();
    if (code != built) {
        table.clear();
        int k = 0;
        for (int i = 0; i < (int)(sizeof(kCodes) / sizeof(kCodes[0])); i++) if (code == kCodes[i]) k = i;
        for (auto &t : kTexts) table[t.es] = (&t.es)[k];
        built = code;
    }
    auto it = table.find(es);
    return it == table.end() ? es : it->second;
}
