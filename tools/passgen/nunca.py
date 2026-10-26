# Test only (tools/passgen/prueba.ps1), with the instance closed: the row Chromium writes when the person answers
# "never" for a site, in a test profile's Login Data. python nunca.py <Login Data> <site, e.g. http://127.0.0.1:9513/>
import sqlite3, sys, time

db, site = sys.argv[1], sys.argv[2]
c = sqlite3.connect(db)
cols = c.execute('pragma table_info(logins)').fetchall()   # (cid, name, type, notnull, default, pk)
row = {'origin_url': site, 'signon_realm': site, 'blacklisted_by_user': 1, 'scheme': 0,
       'date_created': int(time.time() * 1e6) + 11644473600 * 1000000}
for _, name, typ, notnull, default, pk in cols:
    if name in row or pk or not notnull or default is not None:
        continue
    row[name] = b'' if 'BLOB' in (typ or '').upper() else (0 if 'INT' in (typ or '').upper() else '')
names = list(row)
c.execute('insert into logins (%s) values (%s)' % (','.join(names), ','.join('?' * len(names))), [row[n] for n in names])
c.commit()
print('nunca:', site)
