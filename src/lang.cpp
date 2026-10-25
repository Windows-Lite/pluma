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
    // the settings page (settings.cpp, settings.html)
    { L"A los 30 segundos", L"After 30 seconds", L"Após 30 segundos", L"Après 30 secondes", L"Nach 30 Sekunden", L"Dopo 30 secondi" },
    { L"A los 45 segundos", L"After 45 seconds", L"Após 45 segundos", L"Après 45 secondes", L"Nach 45 Sekunden", L"Dopo 45 secondi" },
    { L"A los 2 minutos", L"After 2 minutes", L"Após 2 minutos", L"Après 2 minutes", L"Nach 2 Minuten", L"Dopo 2 minuti" },
    { L"A los 5 minutos", L"After 5 minutes", L"Após 5 minutos", L"Après 5 minutes", L"Nach 5 Minuten", L"Dopo 5 minuti" },
    { L"A los 15 minutos", L"After 15 minutes", L"Após 15 minutos", L"Après 15 minutes", L"Nach 15 Minuten", L"Dopo 15 minuti" },
    { L"Acerca de Pluma", L"About Pluma", L"Sobre o Pluma", L"À propos de Pluma", L"Über Pluma", L"Informazioni su Pluma" },
    { L"Apariencia", L"Appearance", L"Aparência", L"Apparence", L"Darstellung", L"Aspetto" },
    { L"Arrancar con Windows", L"Start with Windows", L"Iniciar com o Windows", L"Démarrer avec Windows", L"Mit Windows starten", L"Avvia con Windows" },
    { L"Borrar", L"Clear", L"Limpar", L"Effacer", L"Löschen", L"Cancella" },
    { L"Buscador de la barra de direcciones", L"Search engine in the address bar", L"Mecanismo de pesquisa da barra de endereços", L"Moteur de recherche de la barre d'adresse", L"Suchmaschine der Adressleiste", L"Motore di ricerca della barra degli indirizzi" },
    { L"Buscar en la configuración", L"Search settings", L"Pesquisar nas configurações", L"Rechercher dans les paramètres", L"Einstellungen durchsuchen", L"Cerca nelle impostazioni" },
    { L"Chrome Web Store", L"Chrome Web Store", L"Chrome Web Store", L"Chrome Web Store", L"Chrome Web Store", L"Chrome Web Store" },
    { L"Código fuente", L"Source code", L"Código-fonte", L"Code source", L"Quellcode", L"Codice sorgente" },
    { L"Dejan de gastar procesador y memoria; se despiertan al volver a ellas", L"They stop using CPU and memory, and wake up when you go back to them", L"Deixam de gastar processador e memória; acordam ao voltar a elas", L"Elles n'utilisent plus le processeur ni la mémoire et se réveillent quand vous y revenez", L"Sie verbrauchen keinen Prozessor und Speicher mehr und wachen auf, wenn du zurückkehrst", L"Smettono di usare processore e memoria; si risvegliano quando ci torni" },
    { L"Desde las versiones publicadas de Pluma, comprobadas antes de instalarse", L"From Pluma's published releases, checked before installing", L"Das versões publicadas do Pluma, verificadas antes de instalar", L"Depuis les versions publiées de Pluma, vérifiées avant l'installation", L"Aus den veröffentlichten Pluma-Versionen, vor der Installation geprüft", L"Dalle versioni pubblicate di Pluma, verificate prima dell'installazione" },
    { L"Dormir las pestañas que no ves", L"Put tabs you aren't looking at to sleep", L"Suspender as guias que não está vendo", L"Mettre en veille les onglets que vous ne regardez pas", L"Nicht sichtbare Tabs schlafen legen", L"Sospendi le schede che non stai guardando" },
    { L"Extensiones y scripts", L"Extensions and scripts", L"Extensões e scripts", L"Extensions et scripts", L"Erweiterungen und Skripte", L"Estensioni e script" },
    { L"Falta el motor de vídeo de Pluma (motor-video)", L"Pluma's video engine (motor-video) is missing", L"Falta o motor de vídeo do Pluma (motor-video)", L"Le moteur vidéo de Pluma (motor-video) est absent", L"Pluma-Videomodul (motor-video) fehlt", L"Manca il motore video di Pluma (motor-video)" },
    { L"Gestionar", L"Manage", L"Gerenciar", L"Gérer", L"Verwalten", L"Gestisci" },
    { L"Hacer predeterminado", L"Make default", L"Tornar padrão", L"Définir par défaut", L"Als Standard festlegen", L"Imposta come predefinito" },
    { L"Idioma de Pluma", L"Pluma's language", L"Idioma do Pluma", L"Langue de Pluma", L"Sprache von Pluma", L"Lingua di Pluma" },
    { L"La barra, los menús y las páginas que lo admiten", L"The bar, the menus and pages that support it", L"A barra, os menus e as páginas que o suportam", L"La barre, les menus et les pages qui le prennent en charge", L"Leiste, Menüs und Seiten, die es unterstützen", L"La barra, i menu e le pagine che lo supportano" },
    { L"La de la barra y los menús de Pluma", L"Used in Pluma's bar and menus", L"A da barra e dos menus do Pluma", L"Celle de la barre et des menus de Pluma", L"Die der Leiste und Menüs von Pluma", L"Quello della barra e dei menu di Pluma" },
    { L"Las pestañas nuevas abren un poco antes, a cambio de algo más de memoria", L"New tabs open a little sooner, using a bit more memory", L"As novas guias abrem um pouco antes, usando um pouco mais de memória", L"Les nouveaux onglets s'ouvrent un peu plus vite, avec un peu plus de mémoire", L"Neue Tabs öffnen etwas schneller, mit etwas mehr Speicher", L"Le nuove schede si aprono un po' prima, usando un po' più di memoria" },
    { L"Las páginas que has visitado", L"The pages you've visited", L"As páginas que você visitou", L"Les pages que vous avez visitées", L"Die Seiten, die du besucht hast", L"Le pagine che hai visitato" },
    { L"Lo que escribas que no sea una dirección se busca aquí", L"Anything you type that isn't an address is searched here", L"O que você digitar que não for um endereço é pesquisado aqui", L"Ce que vous tapez et qui n'est pas une adresse est recherché ici", L"Was du eingibst und keine Adresse ist, wird hier gesucht", L"Ciò che scrivi e non è un indirizzo viene cercato qui" },
    { L"Lo que has descargado", L"What you've downloaded", L"O que você baixou", L"Ce que vous avez téléchargé", L"Was du heruntergeladen hast", L"Ciò che hai scaricato" },
    { L"Los enlaces de otros programas se abren en tu navegador predeterminado", L"Links from other apps open in your default browser", L"Os links de outros programas abrem no seu navegador padrão", L"Les liens d'autres applications s'ouvrent dans votre navigateur par défaut", L"Links aus anderen Programmen öffnen sich im Standardbrowser", L"I link di altri programmi si aprono nel browser predefinito" },
    { L"Los menús y los mensajes de Pluma; las webs lo reciben al abrir Pluma de nuevo", L"Pluma's menus and messages; websites get it after Pluma restarts", L"Os menus e as mensagens do Pluma; os sites o recebem ao abrir o Pluma de novo", L"Les menus et messages de Pluma ; les sites la reçoivent au prochain démarrage", L"Menüs und Meldungen von Pluma; Websites erhalten sie nach einem Neustart", L"Menu e messaggi di Pluma; i siti la ricevono al riavvio di Pluma" },
    { L"Los vídeos usan mucha menos memoria que en la página", L"Videos use much less memory than on the page", L"Os vídeos usam muito menos memória do que na página", L"Les vidéos utilisent bien moins de mémoire que sur la page", L"Videos brauchen viel weniger Speicher als auf der Seite", L"I video usano molta meno memoria che nella pagina" },
    { L"Memoria que usa Pluma ahora", L"Memory Pluma is using now", L"Memória que o Pluma usa agora", L"Mémoire utilisée par Pluma", L"Speicher, den Pluma gerade nutzt", L"Memoria usata ora da Pluma" },
    { L"Mientras no tengas páginas tuyas fijadas en la pestaña nueva", L"While you have no pages of your own pinned to the new tab", L"Enquanto não tiver páginas suas fixadas na nova guia", L"Tant que vous n'avez pas épinglé vos propres pages sur le nouvel onglet", L"Solange du keine eigenen Seiten im neuen Tab angeheftet hast", L"Finché non hai pagine tue fissate nella nuova scheda" },
    { L"Navegador ligero sobre el motor de Edge", L"A light browser on Edge's engine", L"Navegador leve sobre o motor do Edge", L"Navigateur léger sur le moteur d'Edge", L"Leichter Browser auf der Edge-Engine", L"Browser leggero sul motore di Edge" },
    { L"Navegador predeterminado", L"Default browser", L"Navegador padrão", L"Navigateur par défaut", L"Standardbrowser", L"Browser predefinito" },
    { L"No hay nada que coincida", L"Nothing matches", L"Nada corresponde", L"Aucun résultat", L"Keine Treffer", L"Nessun risultato" },
    { L"Pluma es tu navegador predeterminado", L"Pluma is your default browser", L"O Pluma é o seu navegador padrão", L"Pluma est votre navigateur par défaut", L"Pluma ist dein Standardbrowser", L"Pluma è il tuo browser predefinito" },
    { L"Pluma no es tu navegador predeterminado", L"Pluma isn't your default browser", L"O Pluma não é o seu navegador padrão", L"Pluma n'est pas votre navigateur par défaut", L"Pluma ist nicht dein Standardbrowser", L"Pluma non è il tuo browser predefinito" },
    { L"Pluma se abre al instante porque ya está en memoria, escondido", L"Pluma opens instantly because it's already in memory, hidden", L"O Pluma abre na hora porque já está na memória, oculto", L"Pluma s'ouvre instantanément car il est déjà en mémoire, masqué", L"Pluma öffnet sofort, weil es versteckt schon im Speicher ist", L"Pluma si apre subito perché è già in memoria, nascosto" },
    { L"Privacidad y datos", L"Privacy and data", L"Privacidade e dados", L"Confidentialité et données", L"Datenschutz und Daten", L"Privacy e dati" },
    { L"Página de nueva pestaña", L"New tab page", L"Página de nova guia", L"Page Nouvel onglet", L"Seite „Neuer Tab“", L"Pagina Nuova scheda" },
    { L"Seguir en memoria al cerrar la ventana", L"Stay in memory when the window closes", L"Continuar na memória ao fechar a janela", L"Rester en mémoire à la fermeture de la fenêtre", L"Beim Schließen des Fensters im Speicher bleiben", L"Resta in memoria alla chiusura della finestra" },
    { L"Sistema y rendimiento", L"System and performance", L"Sistema e desempenho", L"Système et performances", L"System und Leistung", L"Sistema e prestazioni" },
    { L"Tener una pestaña preparada", L"Keep a tab ready", L"Manter uma guia preparada", L"Garder un onglet prêt", L"Einen Tab bereithalten", L"Tieni pronta una scheda" },
    { L"Ver descargas", L"View downloads", L"Ver downloads", L"Voir les téléchargements", L"Downloads anzeigen", L"Vedi download" },
    { L"Volver a abrirlo es instantáneo; Ctrl+Mayús+Q lo cierra del todo", L"Reopening it is instant; Ctrl+Shift+Q quits it completely", L"Reabrir é instantâneo; Ctrl+Shift+Q o fecha de vez", L"Le rouvrir est instantané ; Ctrl+Maj+Q le ferme complètement", L"Erneutes Öffnen geht sofort; Strg+Umschalt+Q beendet es ganz", L"Riaprirlo è istantaneo; Ctrl+Maiusc+Q lo chiude del tutto" },
    { L"YouTube y vídeo", L"YouTube and video", L"YouTube e vídeo", L"YouTube et vidéo", L"YouTube und Video", L"YouTube e video" },
    // updates (update.cpp)
    { L"Actualizaciones", L"Updates", L"Atualizações", L"Mises à jour", L"Updates", L"Aggiornamenti" },
    { L"Automáticas", L"Automatic", L"Automáticas", L"Automatiques", L"Automatisch", L"Automatici" },
    { L"Avisarme", L"Notify me", L"Avisar-me", L"Me prévenir", L"Benachrichtigen", L"Avvisami" },
    { L"Nunca", L"Never", L"Nunca", L"Jamais", L"Nie", L"Mai" },
    { L"Buscar ahora", L"Check now", L"Verificar agora", L"Rechercher maintenant", L"Jetzt suchen", L"Controlla ora" },
    { L"Versión ", L"Version ", L"Versão ", L"Version ", L"Version ", L"Versione " },
    { L"Nueva versión de Pluma lista: ", L"New version of Pluma ready: ", L"Nova versão do Pluma pronta: ", L"Nouvelle version de Pluma prête : ", L"Neue Pluma-Version bereit: ", L"Nuova versione di Pluma pronta: " },
    { L". Se instala al cerrar la ventana.", L". It installs when you close the window.", L". Será instalada ao fechar a janela.", L". Elle s'installe à la fermeture de la fenêtre.", L". Wird beim Schließen des Fensters installiert.", L". Si installa alla chiusura della finestra." },
    { L"Reiniciar ahora", L"Restart now", L"Reiniciar agora", L"Redémarrer maintenant", L"Jetzt neu starten", L"Riavvia ora" },
    { L"Hay una versión nueva de Pluma: ", L"A new version of Pluma is available: ", L"Há uma nova versão do Pluma: ", L"Une nouvelle version de Pluma est disponible : ", L"Eine neue Version von Pluma ist da: ", L"È disponibile una nuova versione di Pluma: " },
    { L"Ver", L"View", L"Ver", L"Voir", L"Ansehen", L"Vedi" },
    { L"Actualizar y reiniciar", L"Update and restart", L"Atualizar e reiniciar", L"Mettre à jour et redémarrer", L"Aktualisieren und neu starten", L"Aggiorna e riavvia" },
    { L"Descargando la actualización...", L"Downloading the update...", L"Baixando a atualização...", L"Téléchargement de la mise à jour...", L"Update wird heruntergeladen...", L"Download dell'aggiornamento..." },
    { L"No se pudo actualizar", L"Couldn't update", L"Não foi possível atualizar", L"Impossible de mettre à jour", L"Aktualisierung fehlgeschlagen", L"Impossibile aggiornare" },
    { L"Pluma está al día", L"Pluma is up to date", L"O Pluma está atualizado", L"Pluma est à jour", L"Pluma ist aktuell", L"Pluma è aggiornato" },
    { L"Esta copia de Pluma no se actualiza sola", L"This copy of Pluma doesn't update itself", L"Esta cópia do Pluma não se atualiza sozinha", L"Cette copie de Pluma ne se met pas à jour seule", L"Diese Pluma-Kopie aktualisiert sich nicht selbst", L"Questa copia di Pluma non si aggiorna da sola" },
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
