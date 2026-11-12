# u2netp.onnx (Apache-2.0, from the U^2-Net authors via rembg) -> DDM's own little network file (float16 weights)
import onnx, numpy as np, struct, sys
from onnx import numpy_helper as nh
m=onnx.load(sys.argv[1]); g=m.graph
init={t.name: nh.to_array(t) for t in g.initializer}
prod={o:n for n in g.node for o in n.output}
users={}
for n in g.node:
    for i in n.input: users.setdefault(i,[]).append(n)
def attr(n,k,d=None):
    for a in n.attribute:
        if a.name==k: return onnx.helper.get_attribute_value(a)
    return d
ids={}   # tensor name -> id
def tid(name):
    if name not in ids: ids[name]=len(ids)
    return ids[name]
tid(g.input[0].name)
ops=[]; weights=[]
fused=set()
def op(kind, ins, out, params=[]):
    p=list(params)+[0]*(6-len(params))
    return struct.pack('<BII',kind,tid(out) if False else 0,len(ins)) if False else struct.pack('<BI',kind,len(ins))+b''.join(struct.pack('<I',tid(i)) for i in ins)+struct.pack('<I',tid(out))+struct.pack('<6I',*p)
def shapeRef(t):
    # the tensor whose height/width a Resize copies: Shape(x) behind the sizes input
    stack=[t]; seen=set()
    while stack:
        x=stack.pop()
        if x in seen or x not in prod: continue
        seen.add(x); p=prod[x]
        if p.op_type=='Shape' and p.output[0] in seen:
            pass
        if p.op_type=='Cast':  # the height/width part
            stack2=[p.input[0]]
            while stack2:
                y=stack2.pop()
                if y not in prod: continue
                q=prod[y]
                if q.op_type=='Shape': return q.input[0]
                stack2+=list(q.input)
        stack+=list(p.input)
    raise Exception('no ref for '+t)
for n in g.node:
    t=n.op_type
    if t in ('Constant','Shape','Gather','Unsqueeze','Slice','Cast') or (t=='Concat' and attr(n,'axis')==0): continue
    if t=='Relu':
        if n.input[0] in fused: ids[n.output[0]]=ids[n.input[0]]; continue
        raise Exception('lone relu')
    if t=='Conv':
        W=init[n.input[1]]; B=init[n.input[2]]
        oc,ic,kh,kw=W.shape; d=attr(n,'dilations')[0]; p=attr(n,'pads')[0]
        assert attr(n,'group')==1 and attr(n,'strides')==[1,1]
        relu = len(users.get(n.output[0],[]))==1 and users[n.output[0]][0].op_type=='Relu'
        if relu: fused.add(n.output[0])
        off=sum(w.size for w in weights)
        weights.append(W.astype(np.float32).ravel()); weights.append(B.astype(np.float32).ravel())
        ops.append(op(1,[n.input[0]],n.output[0],[ic,oc,kh,d,p,int(relu)]))
        continue
    if t=='MaxPool': ops.append(op(2,[n.input[0]],n.output[0])); continue
    if t=='Resize':
        ref=shapeRef(n.input[3]); ops.append(op(3,[n.input[0],ref],n.output[0])); continue
    if t=='Concat': assert attr(n,'axis')==1; ops.append(op(4,list(n.input),n.output[0])); continue
    if t=='Add': ops.append(op(5,list(n.input),n.output[0])); continue
    if t=='Sigmoid': ops.append(op(6,[n.input[0]],n.output[0])); continue
    raise Exception('op '+t)
out=tid(g.output[0].name)
w=np.concatenate(weights).astype(np.float32)
assert np.isfinite(w).all()
data=b'DDMN'+struct.pack('<IIII',1,len(ops),len(ids),out)+b''.join(ops)+struct.pack('<I',w.size)+w.tobytes()
open(sys.argv[2],'wb').write(data)
print(len(ops),'ops',len(ids),'tensors',w.size,'weights',len(data),'bytes; out',out)
