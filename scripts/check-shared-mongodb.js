// URI 来自环境，不出现在 argv 或输出；仅清理本次探针自己的文档。
try {
    const conn = new Mongo(process.env.REALMMESH_MONGODB_URI);
    const database = conn.getDB('realmmesh');
    const hello = conn.getDB('admin').runCommand({hello: 1});
    if (!hello.isWritablePrimary || hello.setName !== 'rs0') throw new Error('primary unavailable');
    const session = conn.startSession();
    const collection = session.getDatabase('realmmesh').getCollection('__connection_check');
    const id = new ObjectId();
    try {
        session.startTransaction({readConcern: {level: 'majority'}, writeConcern: {w: 'majority', j: true}});
        collection.insertOne({_id: id, checked_at: new Date()});
        session.commitTransaction();
        if (!database.getCollection('__connection_check').findOne({_id: id})) throw new Error('read failed');
        print('MongoDB connection OK: TLS + authentication + rs0 + majority transaction');
    } finally {
        try { session.abortTransaction(); } catch (_) {}
        session.endSession();
        database.getCollection('__connection_check').deleteOne({_id: id}, {writeConcern: {w: 'majority', j: true}});
    }
} catch (error) {
    print('MongoDB connection check failed (' + error.name + ', ' + (error.codeName || error.code || 'no code') + '); inspect private configuration, TLS trust and tunnel.');
    quit(1);
}
